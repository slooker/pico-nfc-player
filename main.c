#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#ifndef MINIMP3_MIN_DATA_CHUNK_SIZE
#define MINIMP3_MIN_DATA_CHUNK_SIZE 4096
#endif
#include "ring_buffer.h"

#ifdef PICO_AUDIO_I2S_ENABLED
#include "pico/audio_i2s.h"
#include "audio_i2s.pio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#endif

// ---- WiFi credentials ----
#define WIFI_SSID     "Lynndral"
#define WIFI_PASSWORD "majortablet239"

// ---- Stream server ----
#define STREAM_HOST "music.slooker.us"
#define STREAM_PORT 443

// ---- NFC tag → Subsonic track map ----

typedef struct {
    uint8_t    uid[7];
    uint8_t    uid_len;
    const char *subsonic_id;
} nfc_entry_t;

static const nfc_entry_t NFC_MAP[] = {
    { {0xAB, 0xCD, 0xEF, 0x12}, 4, "ya2EidN0i7x3byKxDLiWFl" },
    // Add more entries here as you program tags
};
#define NFC_MAP_LEN (sizeof(NFC_MAP) / sizeof(NFC_MAP[0]))

static const nfc_entry_t *nfc_lookup(const uint8_t *uid, uint8_t uid_len) {
    for (size_t i = 0; i < NFC_MAP_LEN; i++) {
        if (NFC_MAP[i].uid_len == uid_len &&
            memcmp(NFC_MAP[i].uid, uid, uid_len) == 0)
            return &NFC_MAP[i];
    }
    return NULL;
}

static void build_stream_path(char *buf, size_t len, const char *track_id) {
    snprintf(buf, len,
        "/rest/stream.view?u=slooker"
        "&t=e9306a305a6b903a07ef53a5bbada0fc"
        "&s=91928c&v=1.16.1&c=PicoPlayer"
        "&format=mp3&maxBitRate=128"
        "&id=%s",
        track_id);
}

// ---- MP3 decoder ----
static mp3dec_t            g_mp3dec;
static mp3dec_frame_info_t g_frame_info;
static int16_t             g_pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];  // 2304 samples

// ---- Ring buffer ----
static ring_buf_t g_ring;
// Scratch used to linearise the ring buffer before each decode call
static uint8_t g_decode_scratch[4096];

// ---- I2S audio (Phase 3 — requires pico-extras) ----
#ifdef PICO_AUDIO_I2S_ENABLED

// Raw I2S state — bypasses pico-extras buffer pool entirely
static uint g_i2s_sm = 0;
static uint g_i2s_dma = 0;

// DMA double-buffer: 2 frames × 1152 stereo pairs = 2304 stereo pairs each
// (52 ms per transfer at 44100 Hz — doubles the window for TLS processing)
static int16_t g_audio_buf[2][2304 * 2];
static uint    g_audio_buf_idx = 0;

// Accumulate two decoded frames before each DMA submit
static int16_t g_audio_accum[2304 * 2];
static uint32_t g_audio_accum_pairs = 0;

static void audio_init(uint32_t sample_rate) {
    // --- PIO setup ---
    int sm = pio_claim_unused_sm(pio0, false);
    if (sm < 0) { printf("No free PIO0 SM\n"); return; }
    g_i2s_sm = (uint)sm;

    gpio_set_function(PICO_AUDIO_I2S_DATA_PIN,          GPIO_FUNC_PIO0);
    gpio_set_function(PICO_AUDIO_I2S_CLOCK_PIN_BASE,     GPIO_FUNC_PIO0);
    gpio_set_function(PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1, GPIO_FUNC_PIO0);

    uint offset = pio_add_program(pio0, &audio_i2s_program);
    audio_i2s_program_init(pio0, g_i2s_sm, offset,
                           PICO_AUDIO_I2S_DATA_PIN,
                           PICO_AUDIO_I2S_CLOCK_PIN_BASE);

    // PIO clock divider: need bit_clock = sample_rate * 32 * 2
    // Each PIO cycle = 1 bit, and the I2S program outputs 32 bits per channel
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t divider = sys_clk * 4 / sample_rate;
    pio_sm_set_clkdiv_int_frac(pio0, g_i2s_sm, divider >> 8u, divider & 0xffu);

    printf("PIO0 SM%u loaded at offset %u, divider=%u\n", g_i2s_sm, offset, divider);

    // --- DMA setup ---
    int dma_ch = dma_claim_unused_channel(false);
    if (dma_ch < 0) { printf("No free DMA ch\n"); return; }
    g_i2s_dma = (uint)dma_ch;

    dma_channel_config dc = dma_channel_get_default_config(g_i2s_dma);
    channel_config_set_dreq(&dc, DREQ_PIO0_TX0 + g_i2s_sm);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    dma_channel_configure(g_i2s_dma, &dc,
                          &pio0->txf[g_i2s_sm],  // dest: PIO TX FIFO
                          NULL,                   // src set per transfer
                          0,                      // count set per transfer
                          false);                 // don't start yet

    printf("DMA ch%u -> PIO0 TX FIFO SM%u (DREQ %u)\n",
           g_i2s_dma, g_i2s_sm, DREQ_PIO0_TX0 + g_i2s_sm);

    // --- Unmute PCM5100A (XSMT = GP22 on PIM544) ---
    gpio_init(22);
    gpio_set_dir(22, GPIO_OUT);
    gpio_put(22, 1);
    printf("PCM5100A unmuted (GP22 HIGH)\n");

    // Enable PIO SM
    pio_sm_set_enabled(pio0, g_i2s_sm, true);
    printf("Audio init done: %luHz\n", (unsigned long)sample_rate);
    stdio_flush();
}

// Submit one block of stereo PCM16 samples via polling DMA (no IRQ needed).
// Uses double buffering: fills the idle buffer while DMA plays the other.
static void audio_submit_raw(const int16_t *pcm, uint32_t stereo_pairs) {
    uint32_t next_idx = g_audio_buf_idx ^ 1;
    memcpy(g_audio_buf[next_idx], pcm, stereo_pairs * 4);
    // Poll WiFi while waiting so the TCP stack stays alive during the ~22ms DMA transfer
    while (dma_channel_is_busy(g_i2s_dma)) {
        cyw43_arch_poll();
    }
    dma_channel_transfer_from_buffer_now(g_i2s_dma, g_audio_buf[next_idx], stereo_pairs);
    g_audio_buf_idx = next_idx;
}

static void play_test_tone(void) {
    printf("Playing 440Hz test tone for 2 seconds...\n");
    stdio_flush();

    const uint32_t sample_rate  = 44100;
    const uint32_t total_frames = sample_rate * 2;
    const uint32_t half_cycle   = sample_rate / (440 * 2);
    const uint32_t chunk        = 1152;

    uint32_t frames_done = 0, half_pos = 0;
    int16_t  level = 16000;

    // Prime DMA with silence so the first wait_for_finish returns quickly
    memset(g_audio_buf[0], 0, sizeof(g_audio_buf[0]));
    dma_channel_transfer_from_buffer_now(g_i2s_dma, g_audio_buf[0], chunk);
    g_audio_buf_idx = 0;

    while (frames_done < total_frames) {
        uint32_t n = chunk;
        if (frames_done + n > total_frames) n = total_frames - frames_done;

        // Fill the idle buffer
        int16_t *out = g_audio_buf[g_audio_buf_idx ^ 1];
        for (uint32_t i = 0; i < n; i++) {
            out[i * 2]     = level;
            out[i * 2 + 1] = level;
            if (++half_pos >= half_cycle) { level = -level; half_pos = 0; }
        }

        // Wait for current DMA then swap
        dma_channel_wait_for_finish_blocking(g_i2s_dma);
        g_audio_buf_idx ^= 1;
        dma_channel_transfer_from_buffer_now(g_i2s_dma, g_audio_buf[g_audio_buf_idx], n);

        frames_done += n;
        if (frames_done % (chunk * 16) == 0) {
            printf("frames=%lu dma_busy=%d\n",
                   (unsigned long)frames_done,
                   (int)dma_channel_is_busy(g_i2s_dma));
            stdio_flush();
        }
    }
    dma_channel_wait_for_finish_blocking(g_i2s_dma);
    printf("Test tone done.\n");
    stdio_flush();
}

static void audio_submit_frame(int16_t *pcm, int sample_count) {
    // sample_count = total samples (L+R interleaved), so stereo_pairs = sample_count/2
    audio_submit_raw(pcm, (uint32_t)(sample_count / 2));
}

#endif  // PICO_AUDIO_I2S_ENABLED

// ---- Chunked transfer-encoding parser ----
typedef enum {
    CHUNK_HEADER,   // reading "<hex>\r\n"
    CHUNK_DATA,     // consuming chunk payload
    CHUNK_TRAILER,  // consuming "\r\n" after payload
    CHUNK_DONE,
} chunk_state_t;

// ---- HTTP / TLS stream state ----
typedef struct {
    struct altcp_pcb *pcb;
    bool     headers_done;
    bool     is_chunked;
    uint32_t bytes_received;
    uint32_t bytes_rx_unacked; // bytes received but not yet altcp_recved'd
    // chunked parser
    chunk_state_t chunk_state;
    char     chunk_header_buf[16];
    uint8_t  chunk_header_pos;
    uint32_t chunk_remaining;
} stream_state_t;

static stream_state_t g_stream  = {0};
static char           g_stream_path[256];

// Feed raw audio bytes through the chunked parser (or straight to the ring buffer).
static void feed_chunked(const uint8_t *data, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {
        switch (g_stream.chunk_state) {

        case CHUNK_HEADER:
            if (data[i] == '\n') {
                // Terminate and parse the accumulated hex string
                g_stream.chunk_header_buf[g_stream.chunk_header_pos] = '\0';
                // Strip trailing \r if present
                if (g_stream.chunk_header_pos > 0 &&
                    g_stream.chunk_header_buf[g_stream.chunk_header_pos - 1] == '\r')
                    g_stream.chunk_header_buf[--g_stream.chunk_header_pos] = '\0';
                // Strip extensions after ';'
                char *semi = strchr(g_stream.chunk_header_buf, ';');
                if (semi) *semi = '\0';

                g_stream.chunk_remaining = (uint32_t)strtol(g_stream.chunk_header_buf, NULL, 16);
                g_stream.chunk_header_pos = 0;

                if (g_stream.chunk_remaining == 0) {
                    g_stream.chunk_state = CHUNK_DONE;
                } else {
                    g_stream.chunk_state = CHUNK_DATA;
                }
            } else {
                if (g_stream.chunk_header_pos < (uint8_t)(sizeof(g_stream.chunk_header_buf) - 1))
                    g_stream.chunk_header_buf[g_stream.chunk_header_pos++] = (char)data[i];
            }
            i++;
            break;

        case CHUNK_DATA: {
            uint16_t take = len - i;
            if ((uint32_t)take > g_stream.chunk_remaining)
                take = (uint16_t)g_stream.chunk_remaining;
            ring_buf_write(&g_ring, data + i, take);
            g_stream.chunk_remaining -= take;
            i += take;
            if (g_stream.chunk_remaining == 0)
                g_stream.chunk_state = CHUNK_TRAILER;
            break;
        }

        case CHUNK_TRAILER:
            // Consume the '\r' and '\n' after the chunk data
            if (data[i] == '\n')
                g_stream.chunk_state = CHUNK_HEADER;
            i++;
            break;

        case CHUNK_DONE:
            return;
        }
    }
}

static void feed_audio_bytes(const uint8_t *data, uint16_t len) {
    if (g_stream.is_chunked)
        feed_chunked(data, len);
    else
        ring_buf_write(&g_ring, data, len);
}

// ---- lwIP callbacks ----

static err_t stream_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        printf("Stream ended. Total audio bytes: %lu\n", g_stream.bytes_received);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    struct pbuf *curr = p;
    while (curr) {
        const uint8_t *data = (const uint8_t *)curr->payload;
        uint16_t       len  = curr->len;

        if (!g_stream.headers_done) {
            // Scan for end-of-headers marker
            for (int i = 0; i <= (int)len - 4; i++) {
                if (data[i]   == '\r' && data[i+1] == '\n' &&
                    data[i+2] == '\r' && data[i+3] == '\n') {

                    printf("--- HTTP Headers ---\n");
                    for (int j = 0; j < i; j++) putchar(data[j]);
                    printf("\n--- Audio data starting ---\n");

                    // Detect chunked transfer encoding
                    char hdr[512];
                    int  hdr_len = (i < (int)sizeof(hdr) - 1) ? i : (int)sizeof(hdr) - 1;
                    memcpy(hdr, data, hdr_len);
                    hdr[hdr_len] = '\0';
                    if (strstr(hdr, "chunked") || strstr(hdr, "Chunked")) {
                        g_stream.is_chunked  = true;
                        g_stream.chunk_state = CHUNK_HEADER;
                        printf("Transfer-Encoding: chunked detected\n");
                    }

                    g_stream.headers_done = true;
                    uint16_t audio_start  = (uint16_t)(i + 4);
                    uint16_t audio_bytes  = len - audio_start;
                    if (audio_bytes > 0) {
                        feed_audio_bytes(data + audio_start, audio_bytes);
                        g_stream.bytes_received += audio_bytes;
                    }
                    break;
                }
            }
        } else {
            g_stream.bytes_received += len;
            feed_audio_bytes(data, len);

            // Log progress every ~64 KB
            if ((g_stream.bytes_received & ~0xFFFF) !=
                ((g_stream.bytes_received - len) & ~0xFFFF)) {
                printf("Received %lu bytes, ring: %lu available\n",
                       g_stream.bytes_received,
                       ring_buf_available(&g_ring));
            }
        }

        curr = curr->next;
    }

    // Accumulate for gradual acking in the main loop.
    g_stream.bytes_rx_unacked += p->tot_len;
    pbuf_free(p);
    return ERR_OK;
}

static err_t stream_sent(void *arg, struct altcp_pcb *pcb, u16_t len) {
    return ERR_OK;
}

static void stream_err(void *arg, err_t err) {
    printf("Stream error: %d\n", (int)err);
}

static err_t stream_poll(void *arg, struct altcp_pcb *pcb) {
    return ERR_OK;
}

static err_t stream_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    if (err != ERR_OK) {
        printf("TLS connect failed: %d\n", (int)err);
        return err;
    }
    printf("TLS connected! Sending HTTP request...\n");

    char req[512];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: PicoPlayer/1.0\r\n"
        "Accept: audio/mpeg, audio/*\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_stream_path, STREAM_HOST);

    err_t write_err = altcp_write(pcb, req, strlen(req), TCP_WRITE_FLAG_COPY);
    if (write_err != ERR_OK) {
        printf("Write failed: %d\n", (int)write_err);
        return write_err;
    }
    return altcp_output(pcb);
}

static void dns_callback(const char *name, const ip_addr_t *addr, void *arg) {
    if (!addr) {
        printf("DNS lookup failed for %s\n", name);
        return;
    }
    printf("DNS resolved %s -> %s\n", name, ipaddr_ntoa(addr));

    struct altcp_tls_config *tls_config = altcp_tls_create_config_client(NULL, 0);
    if (!tls_config) {
        printf("Failed to create TLS config\n");
        return;
    }

    g_stream.pcb = altcp_tls_new(tls_config, IPADDR_TYPE_V4);
    if (!g_stream.pcb) {
        printf("Failed to create TLS PCB\n");
        return;
    }

    mbedtls_ssl_set_hostname(altcp_tls_context(g_stream.pcb), STREAM_HOST);

    altcp_arg(g_stream.pcb,  &g_stream);
    altcp_recv(g_stream.pcb, stream_recv);
    altcp_sent(g_stream.pcb, stream_sent);
    altcp_err(g_stream.pcb,  stream_err);
    altcp_poll(g_stream.pcb, stream_poll, 10);

    err_t err = altcp_connect(g_stream.pcb, addr, STREAM_PORT, stream_connected);
    if (err != ERR_OK)
        printf("Connect failed: %d\n", (int)err);
}

// Start (or restart) streaming a track by Subsonic ID.
static void start_stream(const char *track_id) {
    build_stream_path(g_stream_path, sizeof(g_stream_path), track_id);
    printf("Starting stream: %s\n", g_stream_path);

    memset(&g_stream, 0, sizeof(g_stream));
    ring_buf_init(&g_ring);
    mp3dec_init(&g_mp3dec);

    ip_addr_t server_addr;
    err_t err = dns_gethostbyname(STREAM_HOST, &server_addr, dns_callback, NULL);
    if (err == ERR_OK)
        dns_callback(STREAM_HOST, &server_addr, NULL);
    else if (err != ERR_INPROGRESS)
        printf("DNS error: %d\n", (int)err);
}

// ---- main ----

int main(void) {
    stdio_init_all();
    while (!stdio_usb_connected()) sleep_ms(100);

    printf("Pico NFC Player starting...\n");

    mp3dec_init(&g_mp3dec);
    ring_buf_init(&g_ring);

    if (cyw43_arch_init()) {
        printf("WiFi init failed!\n");
        return -1;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to WiFi: %s\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_MIXED_PSK, 30000) != 0) {
        printf("WiFi connect failed\n");
        return -1;
    }
    printf("WiFi connected! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));

    // Phase 4 placeholder: in the final design this fires when an NFC tag is scanned.
    // For now, auto-start with the first map entry so Phases 2+3 can be tested.
    start_stream(NFC_MAP[0].subsonic_id);

    bool     audio_started = false;
    uint32_t frame_count   = 0;
    uint32_t total_samples = 0;

    while (true) {
        cyw43_arch_poll();

        // Open the TCP receive window at the decode rate.  We cap each iteration
        // to 512 bytes (roughly one frame) so the server never sends faster than
        // we can buffer, and we always ack even when the ring is empty so the
        // window never stays closed during a brief underrun.
        if (g_stream.pcb && g_stream.bytes_rx_unacked > 0) {
            uint32_t ring_free = ring_buf_free(&g_ring);
            uint32_t to_ack = g_stream.bytes_rx_unacked;
            if (to_ack > ring_free) to_ack = ring_free;
            if (to_ack > 512)      to_ack = 512;   // one frame max per iteration
            if (to_ack > 0) {
                altcp_recved(g_stream.pcb, (u16_t)to_ack);
                g_stream.bytes_rx_unacked -= to_ack;
            }
        }

        // Decode one MP3 frame per loop iteration when enough data is buffered.
        // 512 bytes is just over one 128 kbps frame so minimp3 always has a
        // complete frame; the larger threshold only applies before the first frame.
        uint32_t ring_avail = ring_buf_available(&g_ring);
        uint32_t decode_threshold = audio_started ? 512u : MINIMP3_MIN_DATA_CHUNK_SIZE;

        if (ring_avail >= decode_threshold) {
            uint32_t to_read = (ring_avail < sizeof(g_decode_scratch))
                               ? ring_avail : sizeof(g_decode_scratch);

            ring_buf_peek(&g_ring, g_decode_scratch, to_read);

            int samples = mp3dec_decode_frame(
                &g_mp3dec, g_decode_scratch, (int)to_read,
                g_pcm_buf, &g_frame_info);

            if (g_frame_info.frame_bytes > 0) {
                ring_buf_consume(&g_ring, (uint32_t)g_frame_info.frame_bytes);
            } else if (ring_avail >= MINIMP3_MIN_DATA_CHUNK_SIZE) {
                // Enough data to have found a frame but didn't — corrupted byte,
                // skip it.  Don't skip when ring is low; it just means incomplete.
                ring_buf_consume(&g_ring, 1);
            }

            if (samples > 0) {
                if (!audio_started) {
                    printf("Decoded first frame: %dHz, %dch, %d kbps\n",
                           g_frame_info.hz, g_frame_info.channels,
                           g_frame_info.bitrate_kbps);
#ifdef PICO_AUDIO_I2S_ENABLED
                    audio_init((uint32_t)g_frame_info.hz);
#endif
                    audio_started = true;
                }

                frame_count++;
                total_samples += (uint32_t)samples;

                // Print stats every 50 frames (~1.3 seconds at 38fps)
                if (frame_count % 50 == 0) {
                    uint32_t seconds = total_samples / (uint32_t)g_frame_info.hz;
                    printf("Decoded %lu frames, ~%lu:%02lu, ring: %lu\n",
                           (unsigned long)frame_count,
                           (unsigned long)(seconds / 60),
                           (unsigned long)(seconds % 60),
                           (unsigned long)ring_buf_available(&g_ring));
                }

#ifdef PICO_AUDIO_I2S_ENABLED
                // Accumulate into staging buffer; submit every 2 frames (52 ms)
                // so TLS decryption has twice as long to complete inside the
                // DMA wait before a gap can occur.
                uint32_t new_pairs = (uint32_t)(samples * g_frame_info.channels) / 2;
                memcpy(g_audio_accum + g_audio_accum_pairs * 2,
                       g_pcm_buf, new_pairs * 4);
                g_audio_accum_pairs += new_pairs;
                if (g_audio_accum_pairs >= 2304) {
                    audio_submit_frame(g_audio_accum, (int)(g_audio_accum_pairs * 2));
                    g_audio_accum_pairs = 0;
                }
#endif
            }
        }
    }

    cyw43_arch_deinit();
    return 0;
}
