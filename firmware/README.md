# IoT Visual Monitoring — STM32 Firmware

Embedded firmware for the **B-U585I-IOT02A Discovery Kit** that implements the edge node of an autonomous IoT visual monitoring system.

## Architecture

```text
┌──────────────────────────────────────────────────────────────┐
│                      main.c (Orchestrator)                    │
│                                                                │
│  BOOT → Wi-Fi → MQTT → Poll Schedule → Execute                │
│                                                                │
│  ┌──────────┐  ┌──────────┐  ┌──────────────┐  ┌──────────┐  │
│  │ wifi.c   │  │ camera.c │  │ mqtt_handler  │  │ ota_     │  │
│  │ EMW3080  │  │ OV5640   │  │ MQTT 3.1.1   │  │ update.c │  │
│  │ SPI/TCP  │  │ DCMI/DMA │  │ over TCP     │  │ Dual-Bank│  │
│  └──────────┘  └──────────┘  └──────────────┘  └──────────┘  │
│                                                                │
│  ┌────────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ scheduler.c    │  │ captive_     │  │ wifi_            │   │
│  │ cJSON + RTC    │  │ portal.c     │  │ credentials.c    │   │
│  │ STOP2 sleep    │  │ SoftAP HTTP  │  │ Flash storage    │   │
│  └────────────────┘  └──────────────┘  └──────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

---

## Connecting to Wi-Fi

The firmware uses a **3-tier credential chain** to establish Wi-Fi connectivity on every boot. Each tier is attempted in order — the first successful connection wins.

### Tier 1: Flash-Stored Credentials (Persistent)

If Wi-Fi credentials have been saved to internal flash (via captive portal or MQTT `set_wifi` command), they are loaded first. These survive power cycles, OTA updates, and hard resets.

```text
[  255ms] [INFO] [BOOT] Found stored WiFi credentials: SSID='MyNetwork'
[  270ms] [INFO] [WIFI] Connecting to 'MyNetwork'...
[  5410ms] [INFO] [WIFI] Connected (IP: 192.168.1.42)
```

### Tier 2: Captive Portal (Rescue Mode)

If no flash credentials exist or Tier 1 connection fails, the board **automatically launches a Captive Portal** — a local Wi-Fi hotspot with an embedded configuration page and real-time connection feedback.

#### How to Connect via Captive Portal

1. **The board creates a WiFi hotspot** named `IoT-Setup-XXXX` (where `XXXX` is the end of its MAC address)
2. **Connect your phone/laptop** to the `IoT-Setup-XXXX` network
   - **Password:** `setup123`
3. **A configuration page opens automatically** (captive portal redirect)
   - If it doesn't auto-open, navigate to `http://192.168.10.1` manually
4. **Enter your WiFi SSID and password** on the configuration page
5. **Real-Time 3-Phase Progress Streaming** — After submission, the board streams live state feedback directly to your browser via HTTP chunked encoding:

   | Phase | Step Indicator | States Streamed |
   |-------|---------------|-----------------|
   | **1. Validate** | 🔵 Active | Sending association request → WPA2 handshake → DHCP negotiation (attempt X/15) → Link verification → IP assigned |
   | **2. Save** | 🔵 Active | Writing credentials to flash → CRC32 verification |
   | **3. Reboot** | 🔵 Active | All done — rebooting in 3s |

   The page shows a glassmorphism card with a spinning loader, an animated progress bar, step indicators (Validate → Save → Reboot), and a timestamped log console. On failure, the step indicator turns red and auto-redirects back to the form after 4 seconds.

6. **The board saves credentials to flash**, reboots, and connects to your network

```text
[BOOT] No stored WiFi credentials — starting captive portal
[PORT] SoftAP started: SSID='IoT-Setup-AB12', IP=192.168.10.1
[PORT] HTTP server listening on port 80
[PORT] Waiting for WiFi configuration...
```

> **Note:** Credentials persist across reboots and OTA updates. To reconfigure WiFi remotely, use the `set_wifi` or `erase_wifi` MQTT commands (see below).

### Runtime WiFi Reconfiguration (MQTT)

Once the board is online, WiFi credentials can be changed remotely without physical access:

```json
// Save new credentials + reconnect
{"type": "set_wifi", "ssid": "NewNetwork", "password": "NewPassword"}

// Erase stored credentials + reboot into captive portal
{"type": "erase_wifi"}

// Force-start captive portal (doesn't erase stored creds)
{"type": "start_portal"}
```

---

## Build & Flash

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| `arm-none-eabi-gcc` | 10.3+ | ARM Cortex-M33 cross-compiler |
| STM32CubeProgrammer | 2.14+ | Flash programming via ST-Link |
| B-U585I-IOT02A BSP | Latest | Board support package (HAL + BSP drivers) |
| MB1379 Camera Module | — | OV5640 sensor (plugs into DCMI connector) |

### Build

```bash
cd firmware
make -j8
```

Output: `build/thesis-iot-firmware.bin` (~120 KB, ~11.5% of 1MB bank)

### Flash (First Time)

```bash
# Via STM32CubeProgrammer CLI (ST-Link SWD)
STM32_Programmer_CLI -c port=SWD -w build/thesis-iot-firmware.bin 0x08000000 -v -rst
```

After the first flash, all subsequent updates are delivered **over-the-air** via the CI/CD pipeline — no physical connection needed.

### Monitor Serial Output

```bash
# Via the included monitor script (forwards UART logs to MQTT dashboard)
python monitor.py COM7

# Or any serial terminal at 115200 baud on the ST-Link VCP port
```

---

## File Structure

```text
firmware/
├── Core/
│   ├── Inc/
│   │   ├── firmware_config.h      ← Central configuration (Wi-Fi, server, OTA, watchdog)
│   │   ├── ota_update.h           ← OTA dual-bank flash update API
│   │   ├── wifi.h                 ← Wi-Fi connection + HTTP client API
│   │   ├── mqtt_handler.h         ← MQTT 3.1.1 client API
│   │   ├── app_camera.h           ← Camera capture API (warm/cold)
│   │   ├── scheduler.h            ← Schedule parser + RTC alarm API
│   │   ├── wifi_credentials.h     ← Flash-persistent WiFi credential storage
│   │   ├── captive_portal.h       ← SoftAP + embedded HTTP config page
│   │   └── debug_log.h            ← UART logging macros
│   └── Src/
│       ├── main.c                 ← Application orchestrator (2000+ lines)
│       ├── ota_update.c           ← Dual-bank OTA: download → RAM → flash → swap
│       ├── wifi.c                 ← EMW3080 driver + TCP HTTP POST/GET
│       ├── mqtt_handler.c         ← Bare-metal MQTT 3.1.1 client
│       ├── wifi_credentials.c     ← Flash credential storage (CRC32-validated)
│       ├── captive_portal.c       ← SoftAP captive portal for WiFi provisioning
│       ├── app_camera.c           ← OV5640 DCMI capture (warm + cold paths)
│       ├── scheduler.c            ← cJSON schedule parser + RTC + STOP2
│       └── debug_log.c            ← USART1 init + printf redirect
├── Makefile                       ← ARM GCC build system
├── monitor.py                     ← Serial ↔ MQTT log bridge
└── test_ota.py                    ← OTA trigger test script
```

---

## Communication Protocols

| Channel | Protocol | Direction | Purpose |
|---------|----------|-----------|---------|
| MQTT | `device/stm32/commands` | Server → Board | Command & schedule delivery |
| MQTT | `device/stm32/status` | Board → Server | Status updates, telemetry, OTA progress |
| MQTT | `device/stm32/logs` | Board → Server | Live UART log tunneling |
| HTTP | `POST /api/upload` | Board → Server | Image upload (16KB chunked) |
| HTTP | `GET /api/firmware/version` | Board → Server | OTA version check |
| HTTP | `GET /api/firmware/download` | Board → Server | OTA binary download |
| HTTP | `GET /api/time` | Board → Server | RTC time synchronization |

---

## MQTT Command Reference

All commands are JSON payloads published to `device/stm32/commands`:

| Command | Payload | Description |
|---------|---------|-------------|
| Capture Now | `{"type":"capture_now"}` | Instant image capture + upload |
| Capture Sequence | `{"type":"capture_sequence","delays_ms":[0,2000,5000]}` | Multi-capture at ms offsets |
| Schedule | `{"type":"schedule","tasks":[...]}` | AI-generated task schedule |
| Delete Schedule | `{"type":"delete_schedule"}` | Clear active schedule |
| Firmware Update | `{"type":"firmware_update"}` | Trigger OTA update check |
| Sleep Mode | `{"type":"sleep_mode","enabled":true}` | DEEP_DORMANT (~2µA): WiFi off, STOP2 until next scheduled task |
| Low-Power Mode | `{"type":"low_power_mode","mode":"ps_rest"}` | WIFI_PS_REST: WiFi-associated STOP2 with sub-second wake-on-ping (`"off"` to disable) |
| Ping | `{"type":"ping"}` | LED strobe + acknowledgment |
| Set WiFi | `{"type":"set_wifi","ssid":"...","password":"..."}` | Remote WiFi reconfiguration |
| Erase WiFi | `{"type":"erase_wifi"}` | Erase credentials + reboot to portal |
| Start Portal | `{"type":"start_portal"}` | Force captive portal mode |

---

## Over-The-Air (OTA) Architecture

Dual-bank flash architecture (STM32U585AI, 2×1MB) with automatic rollback:

```text
Bank 1: 0x08000000 – 0x080FFFFF  (active firmware)
Bank 2: 0x08100000 – 0x081FFFFF  (inactive / OTA target)
```

### OTA Pipeline

1. **Check** — `GET /api/firmware/version` (semantic version compare, anti-downgrade)
2. **Quiesce** — Disconnect MQTT, stop camera DCMI DMA, drain SPI bus
3. **Download** — Stream firmware binary to RAM via non-blocking HTTP GET
4. **Verify** — Software CRC32 integrity check before touching flash
5. **Flash** — Erase inactive bank → write from RAM (quadword-aligned)
6. **Swap** — Flip `SWAP_BANK` option bit → system reset into new firmware
7. **Validate** — Boot counter in TAMP backup register (auto-rollback after 3 failures)

### OTA Reliability Hardening

- **Yield-First SPI Receiver Strategy**: Enforces a strict 200ms MIPC yield before subsequent TCP reads, while intentionally blocking on the first chunk to prevent the remote VPS from exhausting the EMW3080's 4KB LwIP buffer.
- **Dynamic UDP Socket Burner Pipeline**: Bypasses the EMW3080 `TIME_WAIT` socket recycling bug by opening an incremental number of dummy UDP sockets per attempt, forcing the AT firmware to assign a fresh local FD for the OTA HTTP stream.
- **MIPC MTU Payload Constraint (`safe_chunk=1000`)**: Explicitly limits the requested SPI block size to 1000 bytes (down from 1460 MSS). If the full 1460 bytes are requested while the EMW3080's buffer is saturated by the internet, the AT firmware fails to allocate an internal IPC packet and silently drops the command, stalling the STM32 for 15s.
- **Server-Side TCP Throttling**: The FastAPI backend explicitly streams the 128KB payload using `StreamingResponse` injected with `asyncio.sleep(0.01)` delays every 2KB. This throttles the egress TCP stream to ~200 KB/s, explicitly preventing the EMW3080's internal LwIP memory pool (PBUFs) from becoming radically exhausted by line-rate server bursts.
- **Non-blocking download**: `MSG_DONTWAIT` polling prevents MIPC layer from blocking CPU for full `SO_RCVTIMEO`, ensuring watchdog refresh every ~50ms
- **Bus isolation**: MQTT disconnected + Camera DMA stopped before download to eliminate SPI contention
- **Autonomous polling**: Background daemon checks server every 60s (`OTA_CHECK_INTERVAL_MS`) as fallback if MQTT push is missed
- **Download-to-RAM**: Firmware buffered in SRAM before flash erase — prevents bricking on network failure mid-write

---

## LED Status Indicators

| State | LED Pattern | Meaning |
|-------|-------------|---------|
| Booting | 🔴+🟢 Solid | Hardware initialization in progress |
| Ready | 🟢 3× Flash | All subsystems initialized, MQTT connected |
| Idle | 🟢 50ms pulse / 3s | Heartbeat — system alive and monitoring |
| Command Received | 🔴+🟢 50ms flash | MQTT command acknowledged |
| Capturing | 🔴 Solid | Camera sensor active (recording indicator) |
| Uploading | 🔴 Solid + 🟢 Flicker | HTTP POST data transfer in progress |
| Portal Starting | 🔴 5× Rapid Blink | Captive portal launching |
| Portal Active | 🔴 1s Blink | Waiting for phone to connect + configure |
| Portal Success | 🟢 6× Rapid Blink | Credentials verified, rebooting |
| OTA Detected | 🔴+🟢 5× Strobe | Firmware update starting |
| OTA Flashing | 🔴+🟢 Solid | Flash erase + write (do not power off) |
| OTA Success | 🟢 Solid 1.5s | Verified — rebooting into new firmware |
| Fatal Error | 🔴 Rapid Blink | Unrecoverable error — will watchdog reset |

---

## Security Hardening (OWASP IoT 2025)

| ID | Finding | Severity | Remediation |
|----|---------|----------|-------------|
| SEC-01 | Hardcoded credentials | Critical | Build-time `-D` injection + flash storage |
| SEC-02 | Unauthenticated MQTT | High | Username/password flags in CONNECT packet |
| SEC-03 | MQTT remaining-length overflow | High | 4-byte limit per MQTT 3.1.1 §2.2.3 |
| SEC-04 | Unvalidated RTC time fields | Medium | Range checks before register write |
| SEC-06 | OTA firmware downgrade | High | Semantic version comparison |
| SEC-07 | No hardware watchdog | High | 16s IWDG on dedicated LSI clock |
| SEC-09 | Watchdog starvation during OTA | High | Non-blocking `MSG_DONTWAIT` SPI polling |
| SEC-10 | Heap fragmentation (cJSON) | Medium | Static 12KB bump allocator |
| SEC-11 | Memory sanitization | Medium | `secure_erase()` for sensitive buffers |

