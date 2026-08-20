/**
 * @file    firmware_config.h
 * @brief   Central compile-time configuration for the IoT monitoring firmware
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * All tuneable parameters in one place. Edit this file to match your
 * deployment environment (Wi-Fi network, server IP, MQTT broker, etc.).
 *
 */

#ifndef __FIRMWARE_CONFIG_H
#define __FIRMWARE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Wi-Fi Configuration
 *
 *  SEC-01 (OWASP IoT I1): Credentials MUST be injected at build time.
 *  Example:  make WIFI_SSID='"MyNet"' WIFI_PASSWORD='"secret"'
 *  Never commit plaintext credentials to source control.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define WIFI_CONNECT_TIMEOUT_MS     10000  /* Per-attempt assoc+DHCP deadline (WiFi_Connect) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Enterprise WiFi Retry Policy (2026-08-20)
 *
 *  The EMW3080 driver cannot report *why* a connect failed via its return
 *  code (MX_WIFI_STATUS_T is only OK/ERROR/TIMEOUT/IO_ERROR/PARAM_ERROR).
 *  We disambiguate ourselves: an active scan for the stored SSID
 *  (MX_WIFI_Scan + MX_WIFI_Get_scan_result) tells us whether the AP is
 *  actually broadcasting in range, independent of the auth handshake.
 *
 *    - SSID NOT seen in scan  -> AP absent (hotspot off / out of range).
 *      Retry FOREVER with capped backoff. Never auto-portal — the stored
 *      credentials are presumed correct and will work again once the AP
 *      reappears.
 *    - SSID SEEN in scan, but WIFI_CREDS_SUSPECT_STRIKES consecutive full
 *      connect+DHCP attempts still fail -> the AP demonstrably exists and
 *      we still can't get on it -> very likely a bad password. Auto-enter
 *      the portal so the operator can fix it, instead of retrying into a
 *      wall forever.
 * ═══════════════════════════════════════════════════════════════════════════ */
/* NOTE: no WIFI_SCAN_TIMEOUT_MS here — MX_WIFI_Scan() is a blocking vendor
 * call already bounded by the driver's own MX_WIFI_SCAN_TIMEOUT (5000ms,
 * mx_wifi.h:91). Duplicating it as a second, unenforced constant here is
 * exactly the "dead config" trap WIFI_CONNECT_TIMEOUT_MS used to be —
 * intentionally not repeating it. */
#define WIFI_CREDS_SUSPECT_STRIKES    3      /* Consecutive AP-found-but-failed attempts -> portal */
#define WIFI_RETRY_BACKOFF_INITIAL_MS 2000   /* First background retry delay */
#define WIFI_RETRY_BACKOFF_MAX_MS     30000  /* Backoff ceiling (doubles each failure up to this) */
#define WIFI_REINIT_AFTER_ATTEMPTS    5      /* Full module DeInit/Init only after this many
                                               * consecutive failed attempts (any reason) — not
                                               * on every single retry, to avoid paying the ~5s
                                               * hardware reset delay repeatedly for a module
                                               * that is still perfectly healthy. */
#define WIFI_PORTAL_SANITY_RECHECK_MS (5U * 60U * 1000U)  /* Background retry of stored creds
                                               * while the creds-suspect portal is open — in
                                               * case it was a fluke. Auto-exits + reboots on
                                               * success. Deliberately infrequent so we don't
                                               * hammer an AP that may be rate-limiting failed
                                               * auth attempts. */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Server Configuration (FastAPI backend)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* NOTE: mx_aton_r() below (wifi.c) parses a dotted-quad IP string directly —
 * there is no DNS resolution anywhere in the firmware's network stack. This
 * MUST stay a raw IP literal; "iot.ciocandco.com" would silently fail to
 * parse and break every network path (upload, MQTT, OTA, time-sync). The
 * DNS name still exists and resolves to this same IP for humans/docs. */
#ifndef SERVER_HOST
#define SERVER_HOST                 "46.62.200.8"
#endif
#define SERVER_PORT                 8000
#define SERVER_UPLOAD_PATH          "/api/upload"
#define SERVER_UPLOAD_URL           "http://" SERVER_HOST ":" "8000" SERVER_UPLOAD_PATH
#define SERVER_TIME_PATH            "/api/time"
/* 2026-08-19: measured via tcpdump on the server during a live 4G-hotspot
 * upload — a single lost packet took the EMW3080's onboard TCP stack 10.1s
 * to recover (board resumed sending only after that gap; server had ACKed
 * an earlier offset the whole time, no RST/FIN, healthy receive window).
 * At the previous 8000ms this was misreported as a send FAILURE before the
 * module's own retransmit had a chance to succeed — the transfer was
 * actually recovering, we just weren't waiting long enough to see it.
 * 12000ms clears the observed 10.1s recovery with margin, while staying
 * safely under the 16s IWDG ceiling (WATCHDOG_TIMEOUT_S) since every send
 * loop that uses this refreshes the watchdog per iteration. Do not raise
 * this above ~14s without also raising WATCHDOG_TIMEOUT_S (max ~16s on
 * IWDG/256) or a stalled first send could reset the board before its own
 * timeout even fires. */
#define HTTP_RESPONSE_TIMEOUT_MS   12000

/* SEC-05: image-upload auth token, same pattern as SEC-01 (WiFi creds) —
 * inject at build time, never commit a real value. Empty = unauthenticated
 * (matches server's own escape hatch when FIRMWARE_UPLOAD_TOKEN is unset).
 * Example: make FIRMWARE_UPLOAD_TOKEN='"secret-token"' */
#ifndef FIRMWARE_UPLOAD_TOKEN
#define FIRMWARE_UPLOAD_TOKEN       ""
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT Broker Configuration
 *
 *  SEC-02: Username/password authentication. Set to empty string "" to
 *  disable (backward-compatible). For production: set real credentials.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MQTT_BROKER_HOST            SERVER_HOST
#define MQTT_BROKER_PORT            1883
#define MQTT_CLIENT_ID              "stm32-iot-cam-01"
#define MQTT_KEEPALIVE_SECONDS      60
#define MQTT_CONNECT_TIMEOUT_MS     10000
#define MQTT_RECV_TIMEOUT_MS        100     /* Non-blocking: poll quickly, don't stall the loop */
#ifndef MQTT_USERNAME
#define MQTT_USERNAME               ""     /* Empty = no auth (development) */
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD               ""     /* Empty = no auth (development) */
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  SRAM Budget & Safety Limits
 *
 *  Enterprise practice: hard compile-time ceiling prevents any configuration
 *  change from silently exceeding physical RAM.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SRAM_TOTAL_BYTES            (768 * 1024)      /* 786,432 bytes — STM32U585AI */
#define RAM_SAFETY_MARGIN_BYTES     (64  * 1024)      /* 65,536 — reserved for stack, heap, BSS, WiFi driver */
#define CAMERA_FRAME_BUFFER_MAX     (SRAM_TOTAL_BYTES - RAM_SAFETY_MARGIN_BYTES)

/* ═══════════════════════════════════════════════════════════════════════════
 *  Camera Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CAMERA_DEFAULT_RESOLUTION   CAMERA_RES_VGA    /* 640×480 */
#define CAMERA_CAPTURE_TIMEOUT_MS   5000

/* ── JPEG Output Mode ─────────────────────────────────── */
/*
 * When CAMERA_JPEG_MODE = 1, the OV5640's built-in JPEG encoder is enabled.
 * The board sends compressed JPEG directly instead of raw RGB565.
 *
 * Benefits:  ~50 KB vs 614 KB upload (12× reduction), ~0.4 s vs 4.5 s per image.
 * Requirements: server upload handler auto-detects JPEG (already supported).
 *
 * Set to 0 to revert to raw RGB565 (for debugging or LLM benchmarks needing raw).
 */
#define CAMERA_JPEG_MODE            0     /* 0 = RGB565, server converts to JPEG */
#define CAMERA_JPEG_QUALITY         4     /* OV5640 QS: 0=best/largest, higher=worse/smaller (4 ≈ 80% quality) */

/*
 * Frame buffer size:
 *   RGB565 VGA  = 640 × 480 × 2 = 614,400 bytes
 *   RGB565 QVGA = 320 × 240 × 2 = 153,600 bytes
 *   JPEG VGA    ≈ 30–80 KB (128 KB provides a safe upper bound)
 *
 * STM32U585AI has 768 KB SRAM — both sizes fit comfortably.
 */
#if CAMERA_JPEG_MODE
#define CAMERA_FRAME_BUFFER_SIZE    (128 * 1024)      /* 131,072 bytes — JPEG VGA (30–80 KB typical) */
#else
#define CAMERA_FRAME_BUFFER_SIZE    (640 * 480 * 2)   /* 614,400 bytes — RGB565 VGA */
#endif

/* ── Fast-Capture Tuning ──────────────────────────────── */
#define CAMERA_WARMUP_FRAMES        3                 /* Frames to discard for AEC convergence (cold start only) */
#define CAMERA_AEC_SETTLE_TIMEOUT_MS 1500             /* Max wait for AEC register convergence */
#define CAMERA_VTS_DEFAULT          0x07D0            /* VTS=2000 lines — ~12fps, 83ms max exposure (night mode extends 4x) */
#define CAMERA_INTER_FRAME_DELAY_MS 10                /* Brief ISP settle between snapshots */
#define CAMERA_WARM_CAPTURE_RETRIES 3                 /* Max snapshot attempts before declaring failure */

/* ── Diagnostics ──────────────────────────────────────── */
#define CAMERA_DIAG_ENABLED         0                 /* 1 = verbose hex dump + pixel scan */
#define STACK_WATERMARK_ENABLED     1                 /* 1 = fill stack with canary + periodic check */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Scheduler Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SCHEDULER_MAX_TASKS         32     /* Must match MAX_SCHEDULED_TASKS */
#define SCHEDULER_MQTT_POLL_MS      50     /* Fast poll — keep loop responsive for commands */
#define SCHEDULER_MQTT_WAIT_TOTAL_S 300    /* Max seconds to wait for initial schedule (5 min) */
#define SCHEDULE_JSON_MAX           2048   /* Max schedule JSON size (bytes) — shared with main.c */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Debug UART Configuration (ST-Link VCP on USART1)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define DEBUG_UART_INSTANCE         USART1
#define DEBUG_UART_BAUDRATE         115200
#define DEBUG_LOG_ENABLED           1      /* Set to 0 to disable all logging */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Low Power Configuration — Adaptive Duty-Cycling (WIFI_PS_REST + DEEP_DORMANT)
 *
 *  Default rest = WIFI_PS_REST: WiFi associated in 802.11 power-save (DTIM),
 *  MCU in STOP2, woken sub-second by the EMW3080 NOTIFY line (PD14/EXTI14) on an
 *  incoming MQTT command. A short RTC keepalive wake services the socket so the
 *  link never times out. This gives ~mA-class energy AND wake-on-ping with no
 *  server queue. DEEP_DORMANT (opt-in, agent-commanded) drops WiFi entirely for
 *  ~2uA, waking only on a scheduled deadline or the B3 button.
 *
 *  REQUIRES the IWDG_STOP option byte = FREEZE (programmed by Watchdog_FreezeInStop
 *  in main.c) or the watchdog resets the board mid-sleep.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define LOW_POWER_MODE_ENABLED      1      /* 1 = real STOP2 sleep (was 0 = active-wait stub) */
#define DEEP_SLEEP_ON_COMPLETE      0      /* 0 = no automatic standby — agent-controlled only */
#define WIFI_POWERSAVE_ENABLED      1      /* 1 = MX_WIFI_station_powersave in WIFI_PS_REST */
#define MQTT_KEEPALIVE_S            45     /* RTC keepalive-wake period; MUST be < broker keepalive (mosquitto default 60s) */
#define ENERGY_REPORT_INTERVAL_MS  60000U /* ms between energy state-time publishes (PS-REST only) */
#define DEEP_DORMANT_ENABLED        1      /* 1 = allow agent-commanded ~2uA dormant mode */
#define REST_POLL_S                 3      /* PS-REST STOP2 wake interval in seconds (keepalive RTC tick) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Heartbeat & Observability (PWR-02, OBS-01)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define STATUS_HEARTBEAT_INTERVAL_MS  5000  /* MQTT status publish interval (ms) */
#define IDLE_BLINK_PERIOD_MS          3000  /* Green LED heartbeat blink period */
#define IDLE_BLINK_ON_MS              50    /* Green LED on duration within blink */
#define SCHEDULE_TIME_WINDOW_S        5     /* Seconds of tolerance for task time matching */

/* ═══════════════════════════════════════════════════════════════════════════
 *  B3 USER Button — Short Press / Long Press
 * ═══════════════════════════════════════════════════════════════════════════ */
#define BUTTON_LONG_PRESS_MS          3000  /* Hold ≥3s = enter captive portal mode (creds untouched) */
#define BUTTON_DEBOUNCE_MS            50    /* Ignore presses shorter than 50ms */

/* Factory-reset gesture (2026-08-20) — deliberately staged so it can never
 * fire from a stray tap or a normal 3s portal-hold:
 *   0s..3s   : (unchanged) portal hold window
 *   3s..8s   : nothing new — still just "force portal" if released
 *   8s..10s  : PENDING-ERASE WARNING — distinct fast RED/GREEN alternate LED.
 *              Nothing destructive yet. Releasing here safely ABORTS with
 *              zero side effects (falls back to the 3s force-portal outcome).
 *   >=10s    : commits WiFiCred_Erase() + reboot into a fresh first-boot
 *              portal. Only the RESET (NRST) button is guaranteed side-
 *              effect-free at all times — this is the only path that can
 *              erase credentials, and it requires ~10s of continuous,
 *              deliberate holding with a clear on-board warning first. */
#define BUTTON_FACTORY_RESET_WARN_MS   8000
#define BUTTON_FACTORY_RESET_COMMIT_MS 10000

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEC-07: Command Rate-Limiting
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CMD_RATE_LIMIT_MS             500   /* Min ms between capture_now commands */

/* ═══════════════════════════════════════════════════════════════════════════
 *  REL-02: MQTT Auto-Reconnect
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MQTT_MAX_PUBLISH_FAILURES     3     /* Consecutive failures before reconnect */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Watchdog Configuration (SEC-07 — OWASP I9)
 *
 *  IWDG provides autonomous hardware reset if the main loop stalls.
 *  LSI ≈ 32 kHz, prescaler /256 → 1 tick ≈ 8ms → 16s max timeout.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define WATCHDOG_ENABLED            1       /* 1 = enable IWDG */
#define WATCHDOG_TIMEOUT_S          16      /* Seconds before reset (max ~16 for IWDG/256) */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Upload Optimization
 * ═══════════════════════════════════════════════════════════════════════════ */
/* 16KB chunks — halve SPI round-trips for 2× throughput. This is the value
 * all published benchmark numbers (results/findings_v3.md) were measured
 * with; do not change it casually.
 *
 * 2026-08-18: image upload proved flaky over a 4G phone hotspot (mss 1410) —
 * "Send failed after 3 retries" at offsets 19862, 22682, 8582 of 614400.
 * Tested 4096 as a fix; it did NOT help, and the random (non-chunk-aligned)
 * failure offsets indicate a lossy/jittery uplink rather than a buffer-size
 * threshold, so the change was reverted. One upload DID succeed on the same
 * link, confirming it's flaky rather than broken. Retest on real WiFi before
 * concluding anything about upload reliability. Overridable for experiments:
 * make HTTP_UPLOAD_CHUNK_SIZE=4096 */
#ifndef HTTP_UPLOAD_CHUNK_SIZE
#define HTTP_UPLOAD_CHUNK_SIZE      16384
#endif
#define HTTP_UPLOAD_MAX_RETRIES     2      /* Retry full POST on socket connect failure */
#define HTTP_UPLOAD_RETRY_DELAY_MS  500    /* Delay between retries */

/* ═══════════════════════════════════════════════════════════════════════════
 *  OTA (Over-The-Air) Firmware Update
 *
 *  Enterprise OTA using STM32U585 dual-bank flash.
 *  Board polls the server for new versions and auto-flashes if available.
 * ═══════════════════════════════════════════════════════════════════════════*/
/* Firmware Versioning */
#ifndef FW_VERSION
#define __FW_VERSION_STR            "1.0.165"
#define FW_VERSION                  __FW_VERSION_STR          /* Current firmware version string */
#endif
#define OTA_CHECK_INTERVAL_MS       (1 * 60 * 1000)   /* Check every 1 minute */
#define OTA_DOWNLOAD_CHUNK_SIZE     2048           /* 2KB chunks — fits in single MIPC frame (2494 payload max) */
#define OTA_MAX_FW_SIZE             (896 * 1024)   /* 896KB max — leave room for vector table */
#define OTA_DOWNLOAD_MAX_RETRIES    5              /* Full download attempts before giving up */
#define OTA_DOWNLOAD_RETRY_BASE_MS  3000           /* Exponential backoff base: 3s → 6s → 12s → 24s */
#define OTA_DOWNLOAD_RETRY_MAX_MS   60000          /* Maximum backoff delay: 60s */
#define OTA_PROGRESS_INTERVAL_BYTES (32 * 1024)    /* MQTT progress update every 32KB */
#define OTA_VERSION_PATH            "/api/firmware/version"
#define OTA_DOWNLOAD_PATH           "/api/firmware/download"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Captive Portal Configuration (WiFi Provisioning)
 *
 *  When no stored credentials exist (or connection fails), the board
 *  starts a SoftAP and serves a local configuration page.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define PORTAL_AP_CHANNEL           6
#define PORTAL_AP_IP                "192.168.10.1"
#define PORTAL_AP_NETMASK           "255.255.255.0"
#define PORTAL_AP_GATEWAY           "192.168.10.1"
#define PORTAL_HTTP_PORT            80
#define PORTAL_DNS_PORT             53
#define PORTAL_MAX_CONNECTIONS      4
#define PORTAL_ACCEPT_TIMEOUT_MS    500

/* Flash storage page for WiFi credentials (last page of Bank 2) */
#define WIFI_CRED_FLASH_ADDR        0x081FE000u  /* Page 127, Bank 2 */
#define WIFI_CRED_MAGIC             0x57494649u  /* "WIFI" in ASCII */

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEC-10/11: Fintech Memory Sanitization & Static Allocation
 * ═══════════════════════════════════════════════════════════════════════════ */
void json_mem_reset(void);

#pragma GCC push_options
#pragma GCC optimize ("O0")
static inline void secure_erase(void *v, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)v;
    while (n--) *p++ = 0;
}
#pragma GCC pop_options

#endif /* __FIRMWARE_CONFIG_H */
