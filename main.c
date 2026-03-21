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
#define MINIMP3_MIN_DATA_CHUNK_SIZE 16384
#endif
#include "ring_buffer.h"

#ifdef PICO_AUDIO_I2S_ENABLED
#include "pico/audio_i2s.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
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

static audio_buffer_pool_t *g_audio_pool;

static void audio_init(uint32_t sample_rate) {
    audio_format_t fmt = {
        .sample_freq  = sample_rate,
        .format       = AUDIO_BUFFER_FORMAT_PCM_S16,
        .channel_count = 2,
    };
    audio_buffer_format_t producer_fmt = {
        .format       = &fmt,
        .sample_stride = 4,  // 2 channels × 2 bytes
    };
    g_audio_pool = audio_new_producer_pool(&producer_fmt, 3, 1152);
    printf("audio pool created\n");

    // Find free PIO SM and DMA channel (CYW43 and USB may already own some)
    extern PIO audio_pio;  // defined inside pico_audio_i2s as pio1
    int free_sm = pio_claim_unused_sm(pio1, false);
    if (free_sm < 0) { printf("No free PIO SM!\n"); return; }
    pio_sm_unclaim(pio1, (uint)free_sm);

    int free_dma = dma_claim_unused_channel(false);
    if (free_dma < 0) { printf("No free DMA channel!\n"); return; }
    dma_channel_unclaim((uint)free_dma);

    printf("Using PIO1 SM%d, DMA ch%d\n", free_sm, free_dma);

    audio_i2s_config_t config = {
        .data_pin       = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .dma_channel    = (uint8_t)free_dma,
        .pio_sm         = (uint8_t)free_sm,
    };
    const audio_format_t *actual = audio_i2s_setup(&fmt, &config);
    printf("audio_i2s_setup done: %p\n", actual);
    audio_i2s_connect(g_audio_pool);
    printf("audio_i2s_connect done\n");
    audio_i2s_set_enabled(true);
    printf("Audio initialised: %luHz stereo\n", (unsigned long)sample_rate);
}

static void audio_submit_frame(int16_t *pcm, int sample_count) {
    audio_buffer_t *buf = take_audio_buffer(g_audio_pool, false);
    if (!buf) return;  // no buffer available, skip frame rather than block
    int16_t *out = (int16_t *)buf->buffer->bytes;
    memcpy(out, pcm, sample_count * sizeof(int16_t));
    buf->sample_count = sample_count / 2;  // stereo pairs
    give_audio_buffer(g_audio_pool, buf);
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

    altcp_recved(pcb, p->tot_len);
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
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000) != 0) {
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

        // Decode one MP3 frame per loop iteration when enough data is buffered.
        if (ring_buf_available(&g_ring) >= MINIMP3_MIN_DATA_CHUNK_SIZE) {
            uint32_t avail   = ring_buf_available(&g_ring);
            uint32_t to_read = (avail < sizeof(g_decode_scratch))
                               ? avail : sizeof(g_decode_scratch);

            ring_buf_peek(&g_ring, g_decode_scratch, to_read);

            int samples = mp3dec_decode_frame(
                &g_mp3dec, g_decode_scratch, (int)to_read,
                g_pcm_buf, &g_frame_info);

            if (g_frame_info.frame_bytes > 0) {
                ring_buf_consume(&g_ring, (uint32_t)g_frame_info.frame_bytes);
            } else if (avail > 0) {
                // No valid sync found — skip one byte and try again next iteration
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

                // Print stats every 500 frames (~13 seconds at 38fps)
                if (frame_count % 500 == 0) {
                    uint32_t seconds = total_samples / (uint32_t)g_frame_info.hz;
                    printf("Decoded %lu frames, ~%lu:%02lu elapsed\n",
                           (unsigned long)frame_count,
                           (unsigned long)(seconds / 60),
                           (unsigned long)(seconds % 60));
                }

#ifdef PICO_AUDIO_I2S_ENABLED
                audio_submit_frame(g_pcm_buf, samples * g_frame_info.channels);
#endif
            }
        }

    }

    cyw43_arch_deinit();
    return 0;
}
