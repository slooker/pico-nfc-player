# Pico NFC Player

NFC-triggered audio player on a Raspberry Pi Pico 2W (RP2350).
Scans an NFC tag → looks up a Subsonic track ID → streams MP3 over HTTPS → decodes and plays via I2S.

---

## Hardware

| Component | Part |
|-----------|------|
| MCU | Raspberry Pi Pico 2W (RP2350 + CYW43) |
| DAC / audio output | Pimoroni Pico Audio Pack PIM544 (PCM5100A) |
| NFC reader | PN532 (SPI or I2C) — Phase 4 |

**Pimoroni Audio Pack pin mapping (fixed by hardware):**

| Signal | GPIO |
|--------|------|
| I2S DIN (data) | GP28 |
| I2S BCK (bit clock) | GP26 |
| I2S LRCK (word select) | GP27 |

---

## Project structure

```
pico-nfc-player/
├── CMakeLists.txt            — build config
├── pico_sdk_import.cmake     — SDK locator
├── pico_extras_import.cmake  — copy here to enable I2S audio (Phase 3)
├── main.c                    — all application logic
├── ring_buffer.h             — 32 KB circular buffer (network → decoder)
├── minimp3.h                 — single-header MP3 decoder (lieff/minimp3)
├── lwipopts.h                — lwIP config (poll mode, ALTCP/TLS)
└── mbedtls_config.h          — TLS 1.2 client config (ECDHE/RSA)
```

---

## Build (VS Code + Pico extension)

1. Open the project folder in VS Code.
2. **Configure:** `Ctrl+Shift+P` → `CMake: Configure`
   You should see in the output:
   ```
   pico-extras not found — building without I2S audio output (Phase 3 disabled)
   ```
   (or the "found" variant if pico-extras is installed — see Phase 3 below)
3. **Build:** click **Build** in the bottom status bar, or `Ctrl+Shift+P` → `CMake: Build`

Output binary: `build/pico_nfc_player.uf2`

---

## Flash

**USB only (no debug probe):**
1. Hold **BOOTSEL** on the Pico, plug in USB, release BOOTSEL.
2. Drag `build/pico_nfc_player.uf2` onto the `RPI-RP2` drive that appears.

**With a Picoprobe / CMSIS-DAP debug probe:**
Click the ▶ **Run** button in the VS Code status bar.

---

## Serial monitor

Install the **Serial Monitor** extension (Microsoft) if not already present.

`Ctrl+Shift+P` → `Serial Monitor: Start Monitoring` → select the Pico's COM port → baud **115200**.

---

## Expected serial output

```
Pico NFC Player starting...
Connecting to WiFi: <ssid>
WiFi connected! IP: 192.168.x.x
Starting stream: /rest/stream.view?...
DNS resolved music.slooker.us -> x.x.x.x
TLS connected! Sending HTTP request...
--- HTTP Headers ---
HTTP/1.1 200 OK
Content-Type: audio/mpeg
...
--- Audio data starting ---
Decoded first frame: 44100Hz, 2ch       ← Phase 2 working
Received 65536 bytes, ring: NNN available
Received 131072 bytes, ring: NNN available
...
```

| Output line | What it confirms |
|-------------|-----------------|
| `WiFi connected` | CYW43 + lwIP working |
| `TLS connected` | mbedTLS handshake succeeded |
| HTTP headers printed | Subsonic server responding |
| `Transfer-Encoding: chunked detected` | Chunked parser will engage |
| `Decoded first frame: 44100Hz, 2ch` | minimp3 decoding correctly |
| `Received N bytes` incrementing | Stream flowing end-to-end |

### Troubleshooting

**`Decoded first frame` never appears**
- Check the HTTP status code in the printed headers (401/403 = auth problem; the "audio" bytes will be HTML).
- If headers show `Transfer-Encoding: chunked` but it wasn't detected, the capitalisation differed — check the exact string in the printout.
- If `ring: N available` hits 32768 and stops growing, the ring is filling faster than it drains. Try `sleep_ms(0)` in the main loop as a quick test.

---

## Implementation phases

### Phase 1 — WiFi + HTTPS streaming ✅
WiFi connect, DNS, TLS handshake, HTTP GET, header parsing, byte counting.

### Phase 2 — MP3 decoding ✅
`minimp3` single-header decoder.  Ring buffer sits between the network callbacks and the decode loop. Each main-loop iteration peeks up to 4 KB from the ring buffer into a flat scratch buffer, calls `mp3dec_decode_frame`, then consumes exactly `frame_bytes`. Handles chunked transfer encoding with a 4-state parser.

### Phase 3 — I2S audio output (requires pico-extras)
PCM5100A DAC on the Pimoroni Audio Pack via `pico_audio_i2s`.

**To enable:**
1. Install pico-extras via the Pico VS Code extension, or clone it:
   ```
   git clone https://github.com/raspberrypi/pico-extras
   ```
2. Copy `pico_extras_import.cmake` from `<pico-extras>/external/` into this project root.
3. Re-run CMake (`Ctrl+Shift+P` → `CMake: Configure`).
   The status line will change to `pico-extras found — I2S audio enabled`.
4. Rebuild and flash.

Audio init is deferred until the first MP3 frame is decoded so the I2S clock is set to the correct sample rate automatically.

### Phase 4 — NFC tag reading (stub)
`NFC_MAP[]` lookup table and `nfc_lookup()` / `build_stream_path()` helpers are in `main.c`.
Currently the player auto-starts `NFC_MAP[0]` on boot. When a PN532 module is wired up, replace that call with the result of an NFC scan.

**Adding a new tag:**
Scan the tag to get its UID, then add a row to `NFC_MAP` in `main.c`:
```c
{ {0x04, 0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56}, 7, "<subsonic-track-id>" },
```

---

## NFC → stream URL

```
https://music.slooker.us/rest/stream.view
  ?u=slooker
  &t=<md5(password+salt)>
  &s=<salt>
  &v=1.16.1
  &c=PicoPlayer
  &id=<subsonic-track-id>
```

Authentication uses the Subsonic token method (MD5 hash, not plaintext password).
