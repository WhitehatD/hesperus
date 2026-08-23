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

/* DHCP-failure fallback: see net_lease.c. The board caches the last
 * lease each SSID actually granted it and re-asserts that address when the
 * AP's DHCP server stops answering (RFC 2131 INIT-REBOOT behaviour). This
 * replaced an earlier fallback that hardcoded Apple's Personal Hotspot
 * subnet — that worked on the one AP it knew about and did nothing
 * anywhere else. Nothing to configure here; the cache is keyed by SSID and
 * populated automatically on the first successful DHCP. */

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

/* 2026-08-21 (live incident): association succeeding but DHCP failing on
 * EVERY attempt, sustained across dozens of retries AND multiple full
 * resets (module reinit, MCU reflash, true USB power removal, phone
 * airplane-mode toggle — all identical failure), is a signature that
 * points at the AP's DHCP server holding some per-client state (stale
 * lease / thrash-protection cooldown for a MAC that keeps re-associating)
 * rather than anything on our side. WIFI_RETRY_BACKOFF_MAX_MS (30s) means
 * the board hammers the AP with a fresh associate+DHCP attempt roughly
 * every 30-40s FOREVER once capped — for an AP-side cooldown, that's
 * exactly the wrong behavior: it may be retriggering the same cooldown
 * timer every attempt, so it can never lapse long enough to clear. Once
 * failures are consistently DHCP-specific (not "AP absent", not a hard
 * auth reject) and sustained past this many attempts, escalate the
 * backoff ceiling much further to give the AP genuine quiet time. Still
 * retries forever — never gives up, per the existing policy above — just
 * much more patiently once this pattern is established. */
#define WIFI_DHCP_STREAK_ESCALATE_AFTER 8       /* Consecutive DHCP-specific failures before escalating */
#define WIFI_DHCP_BACKOFF_MAX_MS         300000 /* Escalated ceiling: 5 minutes */
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
/* REVISED 2026-08-21 (measured, chunked-upload era): 12000 was sized for the
 * OLD failure mode above — a single 10.1s MX_WIFI_CMD_TIMEOUT recovery on
 * 614KB monolithic uploads. With 4KB chunks that budget is now pure waste on
 * the failure path: the socket's own MX_SO_RCVTIMEO (WiFi_TcpConnect) bounds
 * each recv, so a 12s budget just stacks THREE blocking recv calls waiting
 * for a response that is never coming, then retries. Measured live, task 14:
 * an 8380-byte capture spent ~16s of its 18.4s total sitting in exactly that
 * loop ("No response to chunk at offset 0"), then completed in ~2s once it
 * gave up and used a fresh socket.
 *
 * CORRECTED again after measuring the REQUEST leg, not just the response:
 * 5000 was set from the healthy-case response time (140-166ms, tasks 12/13)
 * and was wrong, because it ignored how long the request itself takes to
 * traverse a degraded link. Server access log vs board log, task 20: the
 * board began the upload at 23:22:51 and the server did not receive the
 * chunk until 23:22:56.4 — 5.3s IN FLIGHT — so a 5s budget expired just
 * before delivery, every time. Task 19 shows the same shape: first chunk
 * 5.6s to land, subsequent chunks on the same socket 1.45s. The server
 * answered 200 to every one of those "failed" attempts; nothing was ever
 * lost, the board just stopped listening too early and then re-sent 4KB it
 * had already delivered — adding load to the exact link that was slow.
 *
 * 15000 covers the measured first-chunk latency with ~3x margin. Waiting is
 * nearly free; a spurious resend is not. Note the retry path now probes
 * /api/upload/resume before re-sending (see wifi.c), so even when this
 * budget is exceeded the board skips chunks the server already holds
 * instead of duplicating them.
 *
 * 2026-08-23: 15000 IS STILL NOT ENOUGH — this is the THIRD time this
 * constant has been raised chasing the same shape of bug (5000 -> 12000 ->
 * 15000 -> this). Task 8, board vs server access log, timestamps compared:
 *   board "No response to chunk at offset 0"   19:47:18.0
 *   server received that SAME chunk, 200 OK    19:47:19.269   <- 1s LATER
 *   offsets 4096/8192 then land fast on the SAME socket: 19:47:20.8 / :21.1
 *   offset 12288: board gave up 19:47:38, server got it 19:47:53 (15s later)
 * The server answered 200 to every attempt the board logged as failed —
 * nothing was ever lost. The eventual full failure (task 8, offset 12288,
 * 9 attempts, backoff growing to 8000ms, abandoned after ~143s) is a
 * congestion-collapse spiral we cause ourselves: each timeout re-sends 4KB
 * onto the exact link that's already behind, which only makes the queue the
 * NEXT attempt has to clear longer.
 *
 * 2026-08-23, LATER SAME NIGHT — 45000 WAS THE WRONG DIRECTION. Server-side
 * tcpdump (filtered to the board's IP, live during a stalled upload) shows
 * the real mechanism: the board's outbound segments (~1356B, near the
 * negotiated 1410 MSS) get lost, and the gaps measured on the wire before
 * the board's own resends are 2.3s / 4.000s x3 / ~8s / 16.7s — exactly
 * RFC 6298's DEFAULT initial RTO (1s) doubling on loss: 1,2,4,8,16 — even
 * though the TCP handshake measured real RTT at 50ms. That backoff runs
 * INSIDE the EMW3080's own onboard TCP stack; nothing in this codebase can
 * see it or speed it up. So every previous raise of this constant
 * (5000->12000->15000->45000) was "wait longer", which only lets the module
 * sit deeper in a backoff we don't control — the opposite of helpful.
 *
 * The one thing this code DOES control is when it gives up and forces a
 * FRESH connection — the chunk loop already closes the socket and
 * reconnects on any failure. A fresh SYN cost ~50ms on this link, measured
 * directly in the same capture, and starts the module's RTO state over from
 * its unbacked-off beginning. Waiting past ~1-2 RTOs before abandoning only
 * buys more time inside a ladder we can't influence. 5000 gives real margin
 * over the 50ms RTT and normal jitter while abandoning before the module's
 * own backoff compounds past its first couple of steps.
 * UPLOAD_TOTAL_DEADLINE_MS still bounds the whole operation. */
#define HTTP_RESPONSE_TIMEOUT_MS   5000

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

/* 2026-08-21 bisect switch: when 1, skip every post-BSP_CAMERA_Init sensor
 * register override we add on top of ST's OV5640_Common[] table, so a
 * capture runs on the pure manufacturer baseline. Used to determine
 * empirically whether our own writes are responsible for the sensor
 * clocking but not framing, instead of reasoning about it. */
#define CAMERA_BASELINE_ONLY        0

/* Apply the Linux ov5640_set_power_dvp() register set before each capture.
 * Held OFF by default: these writes are unproven on this board and were
 * added while chasing a failure that predates them. The configuration that
 * demonstrably produced good photos did NOT include them. Turn on only to
 * re-test that hypothesis deliberately. */
#define CAMERA_ASSERT_DVP_REGS      0

/* Boot self-test: release all blocks in SYSTEM_RESET01 / CLOCK_ENABLE01
 * before capturing, to test whether a gated block is what stops framing.
 * RESULT 2026-08-21: released cleanly (0x3001 0x08->0x00, 0x3005 0xF7->0xFF)
 * and capture still failed — eliminated, those were power-on defaults. */
#define CAMERA_SELFTEST_RELEASE_BLOCKS 0

/* Drive the OV5640 XCLK master clock from the MCU via MCO1 on PA8.
 * ST documents XCLK as an MCU output on camera-equipped boards, but this
 * firmware never configured MCO and PA8 is otherwise unused.
 * RESULT 2026-08-21: enabling it changed nothing (sensor core stayed
 * frozen), so PA8 is NOT routed to the camera XCLK net on this board —
 * the MB1379 module clocks itself. Kept behind this flag as a documented
 * dead end so nobody re-derives it. */
#define CAMERA_DRIVE_XCLK_FROM_MCU     0

/* Boot self-test: hold the EMW3080 WiFi module in hardware reset during the
 * camera test, isolating shared-3V3-rail load as a cause of the sensor's
 * core halting while its SCCB block stays alive. */
#define CAMERA_SELFTEST_QUIESCE_WIFI   0

/* Boot self-test: enable the sensor's internal colour-bar generator so the
 * pixel array, lens and exposure are bypassed entirely. Isolates "output
 * engine dead" from "nothing upstream to output". */
#define CAMERA_SELFTEST_TEST_PATTERN   0

/* Run a full capture at boot, before WiFi, logging the result to serial.
 * Makes the camera testable on a board that cannot associate (the boot
 * WiFi retry loop never reaches the main loop's button handler). */
#define CAMERA_BOOT_SELFTEST        0
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
#define CAMERA_AEC_SETTLE_TIMEOUT_MS 3000             /* Max wait for AEC register convergence — now polled before EVERY capture, not just cold start (2026-08-21). Raised 1500->3000 on 2026-08-23: with CAMERA_NIGHT_MODE_ENABLE, dim-scene AEC needs several extended-integration frames to ramp, not just one. */
#define CAMERA_INTER_FRAME_DELAY_MS 10                /* Brief ISP settle between snapshots */
#define CAMERA_WARM_CAPTURE_RETRIES 3                 /* Max snapshot attempts before declaring failure */

/* ── Night Mode (extended integration for dim scenes) ────
 * 2026-08-23 investigation (VLM flagged tasks 17/18/20/21 as consistently
 * underexposed after the 2026-08-21 AEC revert to pure vendor defaults).
 * Root cause identified by register audit, NOT yet hardware-validated
 * (board only available at night — camera needs daylight/lit-room testing):
 *
 * ST's OV5640_Common[] table sets max-exposure ceiling registers
 * (0x3A02/0x3A03 = 60Hz, 0x3A14/0x3A15 = 50Hz) to 0x03D8 = 984 lines, while
 * TIMING_VTS is 0x0440 = 1088 lines — i.e. max exposure is capped to ~90%
 * of ONE frame period. Night mode (0x3A00 bit 2) is never enabled anywhere
 * in this codebase (0x3A00 is absent from OV5640_Common[], so it sits at
 * its POR default 0x78 = night mode OFF). Result: no matter how dark the
 * scene, the sensor can never integrate longer than ~1 frame — it can only
 * compensate with gain (which adds noise, matching the VLM's "high noise"
 * reports) instead of exposure time. This is a deliberate vendor default
 * for smooth live video (constant frame rate); it actively hurts a
 * snapshot-only monitoring device that doesn't care about frame rate.
 *
 * Fix (additive, AUTO-mode only — does not touch gain ceiling or the AEC
 * target band, so bright-scene behavior is unchanged; the loop simply gets
 * more headroom to use only when it needs it): enable night mode and raise
 * both max-exposure pairs to 4x a single frame (984*4 = 3936 = 0x0F60),
 * matching the datasheet's documented night-mode ceiling mechanism
 * (OV5640 datasheet §4.6; i.MX/Linux mainline ov5640.c banding-filter
 * derivation). 4x chosen deliberately conservative vs. e.g. 6x/8x seen in
 * some vendor tables, to bound motion blur risk (also VLM-flagged) on a
 * device that may capture rooms with movement.
 *
 * VALIDATE ON HARDWARE before trusting for the BNAIC demo: confirm no
 * regression in daylight/bright-room captures (must stay AUTO, gain ceiling
 * untouched, so expected: none), and confirm materially brighter captures
 * in dim rooms without unacceptable added motion blur. Flip to 0 to fully
 * revert to the vendor baseline if either check fails. */
#define CAMERA_NIGHT_MODE_ENABLE    1

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
