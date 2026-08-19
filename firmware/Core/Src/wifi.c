/**
 * @file    wifi.c
 * @brief   MXCHIP EMW3080 Wi-Fi Driver — Connection Manager & HTTP Client
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Uses the MX_WIFI BSP driver on the B-U585I-IOT02A Discovery Kit.
 * The EMW3080 is connected via SPI and acts as a network co-processor —
 * it handles the full TCP/IP stack internally, exposing a socket API.
 *
 * Implements:
 *   - WiFi_Init()           → EMW3080 hardware init + firmware version check
 *   - WiFi_Connect()        → WPA2 station join with exponential backoff
 *   - WiFi_IsConnected()    → Link status query
 *   - WiFi_HttpPostImage()  → Raw HTTP POST multipart/form-data over TCP socket
 *   - WiFi_DeInit()         → Graceful shutdown
 */

#include "wifi.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "main.h"
#include "ota_update.h"  /* Firmware_CRC32 — shared with the chunked upload finalizer */

#include "mx_wifi.h"
#include "mx_wifi_io.h"
#include "mx_address.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "mqtt_handler.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Module State
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint8_t s_connected    = 0;
static volatile uint8_t s_initialized  = 0;

/* What the ACTIVE power mode wants 802.11 power-save to be (0=off, 1=on).
 * Uploads force it off for the duration of a transfer and then restore
 * this, so PS-REST/dormant keep their energy behaviour while bulk TX
 * still gets full throughput. Owned here so main.c's low-power code and
 * the upload path can't disagree about the real module state. */
static volatile uint8_t s_powersave_desired = 0;

/* Reusable TX buffer for HTTP requests (header + body boundaries) */
#define HTTP_HEADER_MAX  512
static char s_http_header[HTTP_HEADER_MAX];

/* ── Resumable chunked upload tuning ──────────────────────────────────────
 * 2026-08-19: replaced the old single-POST multipart upload. A 614KB frame
 * over a lossy 4G uplink loses the WHOLE transfer to one bad packet with no
 * way to resume. Splitting into independent chunks bounds that to one
 * chunk, not the whole frame — see the design note above WiFi_HttpPostImage.
 *
 * 32KB chunk size + a fresh TCP connection per chunk, per the empirical
 * finding that the EMW3080's own MIPC layer already fragments every
 * Socket_send() to <=2482 bytes internally regardless of what we pass
 * (mx_wifi.c:1465, MX_WIFI_IPC_PAYLOAD_SIZE-12) — so app-level chunk size
 * only controls blast radius on failure, not wire behavior. A fresh
 * connection per chunk means a chunk that hit the module's internal
 * timeout-then-recover state isn't reused in a state we can't verify. */
#define UPLOAD_CHUNK_WIRE_SIZE       32768u
#define UPLOAD_CHUNK_MAX_RETRIES     8
#define UPLOAD_CHUNK_RETRY_BASE_MS   500
#define UPLOAD_CHUNK_RETRY_MAX_MS    8000

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

WiFiStatus_t WiFi_Init(void)
{
    LOG_INFO(TAG_WIFI, "Initializing EMW3080 Wi-Fi module...");

    /* ── Step 1: Initialize SPI2 peripheral (GPIO + clock via HAL_SPI_MspInit) */
    extern void MX_SPI2_Init(void);
    MX_SPI2_Init();

    /* ── Step 1b: Hardware reset the EMW3080 module.
     *    The ST reference mxwifi_probe() only registers bus IO callbacks —
     *    it does NOT reset the module. We must do it here so the EMW3080
     *    boots fresh before any SPI communication. */
    LOG_DEBUG(TAG_WIFI, "FLOW=%d NOTIFY=%d (before reset)",
              HAL_GPIO_ReadPin(MX_WIFI_SPI_FLOW_PORT, MX_WIFI_SPI_FLOW_PIN),
              HAL_GPIO_ReadPin(MX_WIFI_SPI_IRQ_PORT, MX_WIFI_SPI_IRQ_PIN));

    HAL_GPIO_WritePin(MX_WIFI_RESET_PORT, MX_WIFI_RESET_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(MX_WIFI_RESET_PORT, MX_WIFI_RESET_PIN, GPIO_PIN_SET);
    HAL_Delay(5000);  /* EMW3080 boot time — 5s for fresh firmware first-boot */

    LOG_DEBUG(TAG_WIFI, "FLOW=%d NOTIFY=%d (after reset + 1.2s boot)",
              HAL_GPIO_ReadPin(MX_WIFI_SPI_FLOW_PORT, MX_WIFI_SPI_FLOW_PIN),
              HAL_GPIO_ReadPin(MX_WIFI_SPI_IRQ_PORT, MX_WIFI_SPI_IRQ_PIN));

    /* ── Step 2: Probe — register bus IO callbacks with the MX_WIFI driver. */
    if (mxwifi_probe(NULL) != 0)
    {
        LOG_ERROR(TAG_WIFI, "EMW3080 probe FAILED — Bus IO registration error");
        return WIFI_ERROR_INIT;
    }

    /* ── Step 3: Initialize the MX_WIFI stack.
     *    Internally does:
     *      IO_Init(MX_WIFI_INIT) → HW reset + start SPI txrx loop
     *      mipc_init()           → IPC layer
     *      SYS_VERSION command   → Firmware version check
     *      GET_MAC command       → Read MAC address */
    MX_WIFIObject_t *wifi = wifi_obj_get();
    if (MX_WIFI_Init(wifi) != MX_WIFI_STATUS_OK)
    {
        LOG_ERROR(TAG_WIFI, "EMW3080 MX_WIFI_Init FAILED — SPI handshake error");
        LOG_ERROR(TAG_WIFI, "  Check: module firmware, SPI wiring, 2.4GHz radio");
        return WIFI_ERROR_INIT;
    }

    /* Log firmware version and MAC address for diagnostics */
    LOG_INFO(TAG_WIFI, "EMW3080 firmware: %s", wifi->SysInfo.FW_Rev);
    LOG_INFO(TAG_WIFI, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             wifi->SysInfo.MAC[0], wifi->SysInfo.MAC[1],
             wifi->SysInfo.MAC[2], wifi->SysInfo.MAC[3],
             wifi->SysInfo.MAC[4], wifi->SysInfo.MAC[5]);

    s_initialized = 1;
    LOG_INFO(TAG_WIFI, "EMW3080 initialized OK");
    return WIFI_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection
 * ═══════════════════════════════════════════════════════════════════════════ */

WiFiStatus_t WiFi_Connect(const char *ssid, const char *password)
{
    if (!s_initialized)
    {
        LOG_ERROR(TAG_WIFI, "Cannot connect — module not initialized");
        return WIFI_ERROR_INIT;
    }

    LOG_INFO(TAG_WIFI, "Connecting to '%s'...", ssid);

    /* Enable DHCP — without this, the driver sends static IP config (all zeros) */
    wifi_obj_get()->NetSettings.DHCP_IsEnabled = 1;

    uint32_t backoff_ms = 1000;  /* Exponential backoff: 1s → 2s → 4s */

    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRIES; attempt++)
    {
        LOG_DEBUG(TAG_WIFI, "Attempt %d/%d", attempt, WIFI_CONNECT_RETRIES);

        int32_t ret = MX_WIFI_Connect(
            wifi_obj_get(),
            ssid,
            password,
            MX_WIFI_SEC_AUTO
        );

        if (ret == MX_WIFI_STATUS_OK)
        {
            s_connected = 1;
            LOG_INFO(TAG_WIFI, "Connected to '%s' (attempt %d)", ssid, attempt);

            /* Establish a KNOWN power-save state for normal operation.
             *
             * 2026-08-19: bulk upload crawled at ~1.3 KB/s over a phone
             * hotspot (18KB in 14s) while a laptop on the same AP did
             * ~1 MB/s, with the SPI FLOW line pinned LOW — i.e. the module
             * could not drain what we handed it. In 802.11 power-save the
             * station only transmits around DTIM beacon windows, which
             * throttles sustained TX to a trickle while leaving small
             * sends (MQTT keepalives, HTTP headers) apparently fine.
             * Nothing in this firmware ever disabled it outside the
             * PS-REST feature, so whatever the module's boot default is,
             * we inherited it for every upload.
             *
             * NOTE this deliberately does NOT fight the low-power modes:
             * PS-REST still enables power-save via WiFi_SetPowerSave()
             * when it wants to, and uploads force it off only for the
             * duration of the transfer, restoring the mode's wish after
             * (see WiFi_HttpPostImage). This call just makes "normal"
             * mode mean what it says. */
            WiFi_SetPowerSave(0);

            /* Wait for DHCP to assign an IP address.
             * iPhone hotspots can be very slow (10-30s) to respond to
             * embedded DHCP clients — be patient and yield aggressively. */
            MX_WIFI_IO_YIELD(wifi_obj_get(), 5000);
            Watchdog_Refresh();

            uint8_t ip[4] = {0};
            bool got_ip = false;
            for (int dhcp_wait = 0; dhcp_wait < 15; dhcp_wait++)
            {
                /* EMW3080 Connect API is asynchronous for WPA handshakes.
                 * If the user provided a wrong password, association might succeed 
                 * but the subsequent 4-way handshake will fail and the AP will kick us.
                 * If we lose link layer connectivity, fail fast instead of waiting 35s. */
                if (MX_WIFI_IsConnected(wifi_obj_get()) <= 0)
                {
                    LOG_ERROR(TAG_WIFI, "Link dropped during DHCP (wrong password or AP reject)");
                    break;
                }

                if (MX_WIFI_GetIPAddress(wifi_obj_get(), ip, MC_STATION) == MX_WIFI_STATUS_OK
                    && (ip[0] | ip[1] | ip[2] | ip[3]) != 0)
                {
                    got_ip = true;
                    break;
                }
                /* Yield 2s between retries to process SPI + DHCP exchanges */
                MX_WIFI_IO_YIELD(wifi_obj_get(), 2000);
                Watchdog_Refresh();
            }

            if (got_ip)
            {
                LOG_INFO(TAG_WIFI, "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                return WIFI_OK;
            }
            else
            {
                LOG_ERROR(TAG_WIFI, "DHCP failed — no IP assigned (timeout or link drop)");
                s_connected = 0;
                
                /* CRITICAL: Force the module to tear down the socket and radio
                 * state before we attempt another connection, avoiding state machine locks */
                MX_WIFI_Disconnect(wifi_obj_get());
                MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);
                continue;  /* Retry full connection */
            }
        }

        LOG_WARN(TAG_WIFI, "Attempt %d failed (err=%ld), retrying in %lu ms...",
                 attempt, (long)ret, (unsigned long)backoff_ms);
        HAL_Delay(backoff_ms);
        backoff_ms *= 2;  /* Exponential backoff */
    }

    LOG_ERROR(TAG_WIFI, "All %d connection attempts to '%s' failed", WIFI_CONNECT_RETRIES, ssid);
    return WIFI_ERROR_CONNECT;
}

bool WiFi_IsConnected(void)
{
    if (!s_initialized)
    {
        return false;
    }

    int8_t link_status = MX_WIFI_IsConnected(wifi_obj_get());
    s_connected = (link_status > 0) ? 1 : 0;
    return (s_connected != 0);
}

WiFiStatus_t WiFi_TestConnection(const char *ssid, const char *password, WiFiTest_Callback_t cb)
{
    if (!s_initialized)
    {
        LOG_ERROR(TAG_WIFI, "Cannot test — module not initialized");
        if (cb) cb("Error: Wi-Fi module not initialized", 0);
        return WIFI_ERROR_INIT;
    }

    LOG_INFO(TAG_WIFI, "Testing connection to '%s' (fast fail)...", ssid);
    if (cb) cb("Sending association request...", 10);

    /* Enable DHCP */
    wifi_obj_get()->NetSettings.DHCP_IsEnabled = 1;

    if (cb) cb("Connecting to access point (WPA2)...", 15);

    int32_t ret = MX_WIFI_Connect(
        wifi_obj_get(),
        ssid,
        password,
        MX_WIFI_SEC_AUTO
    );

    if (ret == MX_WIFI_STATUS_OK)
    {
        if (cb) cb("WPA handshake complete! Associated.", 25);

        if (cb) cb("Starting DHCP negotiation...", 30);
        
        uint8_t ip[4] = {0};
        bool got_ip = false;
        
        /* 15 * 1000 = 15s wait max (faster than 35s in WiFi_Connect) */
        for (int dhcp_wait = 0; dhcp_wait < 15; dhcp_wait++)
        {
            if (cb) {
                char msg[80];
                snprintf(msg, sizeof(msg), "Requesting IP address (attempt %d/15)...", dhcp_wait + 1);
                cb(msg, 35 + (dhcp_wait * 3));
            }

            /* Ignore temporary link drops for the first 4 seconds of the loop 
             * to allow WPA 4-way handshake and DHCP state machine to settle. */
            if (dhcp_wait >= 3)
            {
                if (cb) cb("Verifying link stability...", 35 + (dhcp_wait * 3));
                if (MX_WIFI_IsConnected(wifi_obj_get()) <= 0)
                {
                    LOG_ERROR(TAG_WIFI, "Test: Link dropped during DHCP (wrong password or AP reject)");
                    if (cb) cb("Link dropped — AP rejected connection", 90);
                    break;
                }
            }

            if (MX_WIFI_GetIPAddress(wifi_obj_get(), ip, MC_STATION) == MX_WIFI_STATUS_OK
                && (ip[0] | ip[1] | ip[2] | ip[3]) != 0)
            {
                got_ip = true;
                break;
            }
            MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);
        }

        if (got_ip)
        {
            LOG_INFO(TAG_WIFI, "Test SUCCESS. IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            if (cb) {
                char msg[80];
                snprintf(msg, sizeof(msg), "IP assigned: %d.%d.%d.%d — Connection verified!", ip[0], ip[1], ip[2], ip[3]);
                cb(msg, 90);
            }
            /* We leave the station connected. Captive portal will reboot the board anyway. */
            return WIFI_OK;
        }
        else
        {
            LOG_ERROR(TAG_WIFI, "Test FAILED — no IP assigned (timeout)");
            if (cb) cb("Failed: No IP assigned (DHCP timeout)", 90);
            /* Tear down the failed connection so the module is ready for next attempt */
            if (cb) cb("Disconnecting from network...", 95);
            MX_WIFI_Disconnect(wifi_obj_get());
            MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);
            return WIFI_ERROR_CONNECT;
        }
    }

    LOG_ERROR(TAG_WIFI, "Test FAILED — association rejected");
    if (cb) cb("Failed: Could not connect (wrong password?)", 90);
    return WIFI_ERROR_CONNECT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP POST — Multipart Image Upload
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Send a buffer over a WiFi socket, retrying on backpressure with
 *         proper driver servicing, giving up only on a genuine socket error.
 *
 * 2026-08-19 ROOT CAUSE (found live, on the actual 4G hotspot, after the
 * chunked-upload rewrite still failed with "Send failed after 3 retries" at
 * a partial chunk offset — the timeout bump alone did NOT fix this):
 *
 * TWO compounding bugs, not one:
 *
 * 1. flags was WRONG. mx_wifi.h documents MX_WIFI_Socket_send's flags param
 *    as "zero for MXOS". This passed HTTP_RESPONSE_TIMEOUT_MS instead —
 *    which mx_wifi.c forwards verbatim as wire-protocol cp->flags to the
 *    module, NOT as a per-call timeout. The actual internal wait is the
 *    FIXED MX_WIFI_CMD_TIMEOUT (mx_wifi_conf.h, 10000ms), completely
 *    unaffected by whatever we passed here — so raising
 *    HTTP_RESPONSE_TIMEOUT_MS from 8000 to 12000 earlier had ZERO effect on
 *    this call. (This IS what the packet-capture-measured "10.1s recovery"
 *    actually was: MX_WIFI_CMD_TIMEOUT, not a TCP RTO as first assumed.)
 *
 * 2. sent==0 was treated as a hard failure. MX_WIFI_Socket_send's contract
 *    is "Number of bytes sent, return < 0 if failed" — 0 is neither: it's
 *    the module reporting its own outbound buffer is full (normal
 *    backpressure on a slow uplink, exactly like POSIX EWOULDBLOCK). The
 *    old code counted it as a strike and gave up after 3, waiting only
 *    HAL_Delay(50) between attempts — and HAL_Delay is a dead CPU spin.
 *    MX_WIFI_USE_CMSIS_OS=0 (mx_wifi_conf.h) means this is bare-metal: NO
 *    background thread services the SPI/MIPC layer. Only an explicit
 *    MX_WIFI_IO_YIELD() call (-> mipc_poll() -> mx_wifi_hci_recv()) pumps
 *    it. So the 50ms "backoff" serviced nothing, and 150ms total is far too
 *    short for a module's outbound buffer to drain onto a congested 4G
 *    uplink — the transfer was aborted while it was still recoverable.
 *
 * Fix: flags=0 (matches every other correct call site in this codebase —
 * see mqtt_handler.c:225, captive_portal.c:367). Treat sent==0 as patient,
 * properly-yielded backpressure with NO strike limit, bounded instead by a
 * wall-clock ceiling on this one socket; treat sent<0 as a real error with
 * a short bounded retry. Either bound handing back to the CALLER
 * (WiFi_HttpPostImage's per-chunk loop), which opens a fresh connection and
 * retries — a fresh socket is more likely to recover a truly wedged module
 * state than continuing to hammer this one.
 *
 * @retval 0 on success, -1 on error or a stalled connection (caller should
 *         open a fresh socket and retry).
 */
static int _socket_send_all(int32_t sock, const uint8_t *data, int32_t len)
{
    int32_t offset = 0;
    int hard_errors = 0;
    uint32_t zero_sends = 0;   /* module said "buffer full" (backpressure) */
    uint32_t neg_sends  = 0;   /* module reported an actual error */
    uint32_t last_mqtt_tick = HAL_GetTick();
    uint32_t start_tick = HAL_GetTick();

    while (offset < len)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu; /* Prevent 16s watchdog reset during long sends */
#endif
        /* Bail out of THIS socket (not the whole upload) if it's been
         * stuck too long — well under the 16s IWDG ceiling, and the
         * caller's per-chunk retry opens a fresh connection on -1 rather
         * than looping here forever. */
        if ((HAL_GetTick() - start_tick) > 14000)
        {
            /* Diagnostics, not decoration: which failure mode actually
             * wedged us? zero_sends = module reported "buffer full"
             * (backpressure); neg_sends = module reported an error;
             * FLOW low = the module is telling the SPI layer it cannot
             * accept ANY more bytes, i.e. the wedge is below the socket
             * API entirely (mx_wifi_spi.c:wait_flow_high). These three
             * numbers distinguish "slow uplink" from "module TX path
             * hung" — we were guessing between them for hours. */
            LOG_WARN(TAG_HTTP, "Send stalled >14s at offset %ld/%ld "
                     "[zero_sends=%lu neg_sends=%lu flow=%s] — fresh-connection retry",
                     (long)offset, (long)len,
                     (unsigned long)zero_sends, (unsigned long)neg_sends,
                     (HAL_GPIO_ReadPin(MX_WIFI_SPI_FLOW_PORT, MX_WIFI_SPI_FLOW_PIN)
                        == GPIO_PIN_RESET) ? "LOW(blocked)" : "HIGH(ready)");
            return -1;
        }

        int32_t chunk = len - offset;
        if (chunk > HTTP_UPLOAD_CHUNK_SIZE)
            chunk = HTTP_UPLOAD_CHUNK_SIZE;

        /* flags MUST be 0 — see the root-cause note above. */
        int32_t sent = MX_WIFI_Socket_send(
            wifi_obj_get(), sock, (uint8_t *)(data + offset), chunk, 0);

        if (sent > 0)
        {
            BSP_LED_Toggle(LED_GREEN);
            offset += sent;
            hard_errors = 0;

            if (offset < len)
                MX_WIFI_IO_YIELD(wifi_obj_get(), 2);

            /* Keep MQTT alive during long uploads.
             * Send PINGREQ every 5s to prevent broker keepalive timeout (60s).
             * We intentionally do NOT call MQTT_ProcessLoop() here — its 1s
             * recv timeout stalls the upload, and the SPI bus contention during
             * heavy HTTP traffic causes incoming MQTT messages to be dropped
             * by the EMW3080's limited buffer. Commands are processed naturally
             * after the upload completes. */
            if ((HAL_GetTick() - last_mqtt_tick) > 5000)
            {
                MQTT_SendPing();
                last_mqtt_tick = HAL_GetTick();
            }
        }
        else if (sent == 0)
        {
            /* Backpressure, not failure — module's outbound buffer is full.
             * Yield so the driver is actually SERVICED (bare-metal, nothing
             * else pumps it), then retry the same bytes. No strike limit
             * here on purpose: a link recovering in 2s shouldn't be treated
             * the same as a dead one — the 14s wall-clock check above is
             * the real bound. */
            zero_sends++;
            MX_WIFI_IO_YIELD(wifi_obj_get(), 20);
        }
        else
        {
            /* sent < 0: a genuine module-reported error, not backpressure.
             * Short bounded retry — if it keeps happening the socket is
             * likely actually broken, and the wall-clock check (or this)
             * hands it to the caller's fresh-connection retry, which can
             * actually fix that. */
            hard_errors++;
            neg_sends++;
            if (hard_errors >= 5)
            {
                LOG_ERROR(TAG_HTTP, "Socket error at offset %ld/%ld (rc=%ld, %d consecutive, "
                          "zero_sends=%lu)", (long)offset, (long)len, (long)sent,
                          hard_errors, (unsigned long)zero_sends);
                return -1;
            }
            MX_WIFI_IO_YIELD(wifi_obj_get(), 50);
        }
    }
    return 0;
}

void WiFi_SetPowerSave(uint8_t enable)
{
    if (!s_initialized)
        return;

    s_powersave_desired = enable ? 1u : 0u;
    MX_WIFI_station_powersave(wifi_obj_get(), (int32_t)s_powersave_desired);
    LOG_DEBUG(TAG_WIFI, "802.11 power-save -> %s", s_powersave_desired ? "ON" : "OFF");
}

/**
 * @brief  Force power-save off for a bulk transfer, returning the previous
 *         desired state so the caller can restore it afterwards.
 *
 * In 802.11 power-save the station only transmits around DTIM windows,
 * which collapses sustained upload throughput (measured ~1.3 KB/s vs
 * ~1 MB/s for a laptop on the same AP) and leaves the SPI FLOW line low
 * because the module cannot drain its TX buffer. Transfers must run with
 * it off; the low-power modes get their setting back the moment we're done.
 */
static uint8_t _powersave_suspend_for_transfer(void)
{
    uint8_t previous = s_powersave_desired;
    if (previous)
    {
        MX_WIFI_station_powersave(wifi_obj_get(), 0);
        LOG_INFO(TAG_WIFI, "power-save suspended for transfer (will restore)");
    }
    return previous;
}

static void _powersave_restore(uint8_t previous)
{
    if (previous)
    {
        MX_WIFI_station_powersave(wifi_obj_get(), 1);
        LOG_DEBUG(TAG_WIFI, "power-save restored");
    }
    s_powersave_desired = previous;
}

/**
 * @brief  Parse the HTTP status code from a raw response buffer's status
 *         line ("HTTP/1.1 200 OK..." → 200). Shared by every response
 *         parser in this file (was previously duplicated inline).
 * @retval Status code, or 0 if the line couldn't be parsed.
 */
static int _parse_http_status(const uint8_t *resp_buf)
{
    int http_code = 0;
    const char *space = strchr((const char *)resp_buf, ' ');
    if (space != NULL)
    {
        char *endptr = NULL;
        long parsed = strtol(space + 1, &endptr, 10);
        if (endptr != space + 1 && parsed >= 100 && parsed <= 599)
            http_code = (int)parsed;
    }
    return http_code;
}

/**
 * @brief  Locate the JSON body within a raw HTTP response, skipping past
 *         headers exactly the way the time-sync parser already does
 *         (uvicorn may add chunked-transfer-encoding size lines before the
 *         actual JSON — skip to \r\n\r\n, then find the first '{').
 * @retval Pointer to the opening '{', or NULL if none found.
 */
static const char *_find_json_body(const uint8_t *resp_buf)
{
    const char *hdr_end = strstr((const char *)resp_buf, "\r\n\r\n");
    if (hdr_end == NULL)
        return NULL;
    return strchr(hdr_end + 4, '{');
}

/**
 * Resumable chunked image upload to the FastAPI server.
 *
 * 2026-08-19: replaced the old single-POST multipart upload. Packet capture
 * during a real 4G-hotspot failure showed the board's TCP stack recovering
 * from a single lost packet in ~10.1s — longer than the previous 8s
 * per-send timeout, so genuine (if slow) recovery was misreported as
 * failure. Even with that timeout fixed (HTTP_RESPONSE_TIMEOUT_MS), one
 * monolithic 614KB POST still threw away ALL progress on any connection
 * that couldn't be salvaged and restarted from byte 0 into the same
 * conditions — on a sustained-loss link that never converges.
 *
 * This sends the image as independent ~32KB chunks (UPLOAD_CHUNK_WIRE_SIZE),
 * each its own short-lived POST to /api/upload/chunk carrying its byte
 * offset — see server/app/api/routes.py's matching endpoint. A stalled
 * connection costs one chunk, not the whole frame: on failure we retry
 * the SAME chunk (never advancing offset) with exponential backoff, over a
 * FRESH TCP connection each attempt (a wedged EMW3080 socket's internal
 * state after a timeout is not something we trust to reuse — see the
 * per-send MIPC fragmentation note above UPLOAD_CHUNK_WIRE_SIZE).
 *
 * Wire format per chunk (raw bytes, no multipart — the server addresses
 * chunks by byte offset, so there's nothing multipart framing would add):
 *   POST /api/upload/chunk?task_id=N&offset=O&total_size=T HTTP/1.1\r\n
 *   Host: <server>\r\n
 *   X-Upload-Token: <token>\r\n
 *   Content-Type: application/octet-stream\r\n
 *   Content-Length: <chunk_len>\r\n
 *   Connection: close\r\n
 *   \r\n
 *   <raw chunk bytes>
 *
 * The server responds with {"task_id":N,"received_offset":O2,"total_size":T}
 * — we trust ITS reported offset (not just "the send succeeded") before
 * advancing, and adopt its task_id (handles the fallback-ID remap that
 * happens server-side for unprompted/button captures, exactly once, on the
 * very first chunk — see /api/upload/chunk's docstring).
 *
 * After every chunk is confirmed, POST /api/upload/complete with a CRC32
 * of the full buffer (Firmware_CRC32, the same MPEG-2 variant already used
 * for OTA — one CRC convention, not two that could drift) so the server
 * can catch a chunk landing at the wrong offset or a partial write before
 * treating the frame as valid image data.
 */
WiFiStatus_t WiFi_HttpPostImage(const char *url, uint32_t task_id,
                                 const uint8_t *data, uint32_t data_len)
{
    (void)url;  /* We construct the request from config constants */
    uint32_t upload_start_tick = HAL_GetTick();

    if (!s_connected)
    {
        LOG_ERROR(TAG_HTTP, "Cannot POST — Wi-Fi not connected");
        return WIFI_ERROR_SEND;
    }

    if (data == NULL || data_len == 0)
    {
        LOG_ERROR(TAG_HTTP, "Cannot POST — no image data");
        return WIFI_ERROR_SEND;
    }

    LOG_INFO(TAG_HTTP, "Chunked upload starting: %lu bytes, task %lu",
             (unsigned long)data_len, (unsigned long)task_id);

    /* Bulk TX cannot share the link with 802.11 power-save — see
     * _powersave_suspend_for_transfer(). Restored on EVERY exit path below. */
    uint8_t ps_prev = _powersave_suspend_for_transfer();

    uint32_t resolved_task_id = task_id;
    uint32_t offset = 0;
    uint32_t consecutive_failures = 0;
    uint32_t backoff_ms = UPLOAD_CHUNK_RETRY_BASE_MS;

    /* ── Phase 1: send every chunk, resuming from the server's confirmed
     *            offset on any failure rather than restarting from 0 ── */
    while (offset < data_len)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;
#endif
        uint32_t remaining = data_len - offset;
        uint32_t chunk_len = (remaining < UPLOAD_CHUNK_WIRE_SIZE) ? remaining : UPLOAD_CHUNK_WIRE_SIZE;

        int header_len = snprintf(s_http_header, HTTP_HEADER_MAX,
            "POST %s/chunk?task_id=%lu&offset=%lu&total_size=%lu HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "X-Upload-Token: %s\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n",
            SERVER_UPLOAD_PATH, (unsigned long)resolved_task_id, (unsigned long)offset,
            (unsigned long)data_len, SERVER_HOST, SERVER_PORT, FIRMWARE_UPLOAD_TOKEN,
            (unsigned long)chunk_len);

        bool chunk_ok = false;
        uint32_t confirmed_offset = offset;
        uint32_t confirmed_task_id = resolved_task_id;
        bool restart_from_zero = false;

        int32_t sock = WiFi_TcpConnect(SERVER_HOST, SERVER_PORT);
        if (sock < 0)
        {
            LOG_WARN(TAG_HTTP, "Chunk connect failed at offset %lu/%lu",
                     (unsigned long)offset, (unsigned long)data_len);
        }
        else
        {
            WiFiStatus_t send_status = WIFI_OK;
            if (_socket_send_all(sock, (uint8_t *)s_http_header, header_len) != 0)
                send_status = WIFI_ERROR_SEND;
            if (send_status == WIFI_OK &&
                _socket_send_all(sock, data + offset, (int32_t)chunk_len) != 0)
                send_status = WIFI_ERROR_SEND;

            if (send_status == WIFI_OK)
            {
                /* Poll with yields, not a single-shot recv — the EMW3080
                 * delivers the response asynchronously over SPI, so it may
                 * not have arrived at the exact instant the send finished.
                 * A single-shot call here would misreport "no response" for
                 * a chunk the server actually processed fine, forcing a
                 * wasted fresh-connection retry. Matches the pre-existing
                 * time-sync response reader below. */
                uint8_t resp_buf[768] = {0};
                int32_t resp_len = 0;
                uint32_t resp_wait_start = HAL_GetTick();
                while ((HAL_GetTick() - resp_wait_start) < HTTP_RESPONSE_TIMEOUT_MS)
                {
                    MX_WIFI_IO_YIELD(wifi_obj_get(), 50);
                    resp_len = MX_WIFI_Socket_recv(
                        wifi_obj_get(), sock, resp_buf, sizeof(resp_buf) - 1, 0);
                    if (resp_len > 0)
                        break;
                }

                if (resp_len > 0)
                {
                    resp_buf[resp_len] = '\0';
                    int http_code = _parse_http_status(resp_buf);

                    if (http_code >= 200 && http_code < 300)
                    {
                        const char *body = _find_json_body(resp_buf);
                        if (body != NULL)
                        {
                            json_mem_reset();
                            cJSON *root = cJSON_Parse(body);
                            if (root != NULL)
                            {
                                cJSON *j_tid = cJSON_GetObjectItem(root, "task_id");
                                cJSON *j_off = cJSON_GetObjectItem(root, "received_offset");
                                if (cJSON_IsNumber(j_tid))
                                    confirmed_task_id = (uint32_t)j_tid->valuedouble;
                                if (cJSON_IsNumber(j_off))
                                {
                                    confirmed_offset = (uint32_t)j_off->valuedouble;
                                    chunk_ok = true;
                                }
                                cJSON_Delete(root);
                            }
                        }
                    }
                    else if (http_code == 409)
                    {
                        /* Server has no record of this upload (restarted
                         * since, or its stale-upload sweep already claimed
                         * it) — nothing to resume, start a fresh session
                         * under the same task_id from byte 0. */
                        LOG_WARN(TAG_HTTP, "Server lost upload state (409) — restarting from offset 0");
                        restart_from_zero = true;
                    }
                    else
                    {
                        LOG_WARN(TAG_HTTP, "Chunk POST returned HTTP %d", http_code);
                    }
                }
                else
                {
                    LOG_WARN(TAG_HTTP, "No response to chunk at offset %lu (timeout or closed)",
                             (unsigned long)offset);
                }
            }

            MX_WIFI_Socket_close(wifi_obj_get(), sock);
        }

        if (restart_from_zero)
        {
            offset = 0;
            consecutive_failures = 0;
            backoff_ms = UPLOAD_CHUNK_RETRY_BASE_MS;
            continue;
        }

        if (chunk_ok && confirmed_offset > offset)
        {
            resolved_task_id = confirmed_task_id;
            offset = confirmed_offset;
            consecutive_failures = 0;
            backoff_ms = UPLOAD_CHUNK_RETRY_BASE_MS;
            BSP_LED_Toggle(LED_GREEN);

            /* Keep MQTT alive during a long upload — same reasoning as
             * before: ProcessLoop's 1s recv timeout would stall chunk
             * sends, so ping only, don't run the full loop here. */
            MQTT_SendPing();
            continue;
        }

        /* Chunk failed (connect, send, timeout, bad status, or a
         * duplicate ack that didn't advance the offset) — back off and
         * retry the SAME chunk. offset never advances on failure, so a
         * retry re-sends exactly the bytes the server is still missing. */
        consecutive_failures++;
        if (consecutive_failures > UPLOAD_CHUNK_MAX_RETRIES)
        {
            LOG_ERROR(TAG_HTTP, "Chunk upload abandoned after %lu failures at offset %lu/%lu",
                      (unsigned long)consecutive_failures, (unsigned long)offset,
                      (unsigned long)data_len);
            BSP_LED_Off(LED_GREEN);
            _powersave_restore(ps_prev);
            return WIFI_ERROR_SEND;
        }

        LOG_WARN(TAG_HTTP, "Chunk retry %lu/%d after %lums (offset %lu/%lu)...",
                 (unsigned long)consecutive_failures, UPLOAD_CHUNK_MAX_RETRIES,
                 (unsigned long)backoff_ms, (unsigned long)offset, (unsigned long)data_len);
        HAL_Delay(backoff_ms);
        backoff_ms = (backoff_ms * 2 > UPLOAD_CHUNK_RETRY_MAX_MS) ? UPLOAD_CHUNK_RETRY_MAX_MS : backoff_ms * 2;
        MQTT_SendPing();
    }

    /* ── Phase 2: every chunk confirmed by the server — finalize with a
     *            whole-payload CRC32 so it can catch a chunk landing at
     *            the wrong offset before treating the frame as valid ── */
    uint32_t crc = Firmware_CRC32(0xFFFFFFFF, data, data_len);
    uint32_t complete_failures = 0;
    uint32_t complete_backoff_ms = UPLOAD_CHUNK_RETRY_BASE_MS;

    for (;;)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;
#endif
        int header_len = snprintf(s_http_header, HTTP_HEADER_MAX,
            "POST %s/complete?task_id=%lu&total_size=%lu&crc32=%lu HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "X-Upload-Token: %s\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n",
            SERVER_UPLOAD_PATH, (unsigned long)resolved_task_id, (unsigned long)data_len,
            (unsigned long)crc, SERVER_HOST, SERVER_PORT, FIRMWARE_UPLOAD_TOKEN);

        bool done_ok = false;
        int32_t sock = WiFi_TcpConnect(SERVER_HOST, SERVER_PORT);
        if (sock >= 0)
        {
            if (_socket_send_all(sock, (uint8_t *)s_http_header, header_len) == 0)
            {
                /* Poll with yields — see the matching comment on the chunk
                 * response reader above; same asynchronous-delivery reason. */
                uint8_t resp_buf[512] = {0};
                int32_t resp_len = 0;
                uint32_t resp_wait_start = HAL_GetTick();
                while ((HAL_GetTick() - resp_wait_start) < HTTP_RESPONSE_TIMEOUT_MS)
                {
                    MX_WIFI_IO_YIELD(wifi_obj_get(), 50);
                    resp_len = MX_WIFI_Socket_recv(
                        wifi_obj_get(), sock, resp_buf, sizeof(resp_buf) - 1, 0);
                    if (resp_len > 0)
                        break;
                }
                if (resp_len > 0)
                {
                    resp_buf[resp_len] = '\0';
                    int http_code = _parse_http_status(resp_buf);
                    if (http_code >= 200 && http_code < 300)
                    {
                        done_ok = true;
                    }
                    else
                    {
                        LOG_WARN(TAG_HTTP, "Upload complete returned HTTP %d: %.80s",
                                 http_code, (char *)resp_buf);
                    }
                }
                else
                {
                    LOG_WARN(TAG_HTTP, "No response to /complete (timeout or closed)");
                }
            }
            else
            {
                LOG_WARN(TAG_HTTP, "Failed to send /complete request");
            }
            MX_WIFI_Socket_close(wifi_obj_get(), sock);
        }
        else
        {
            LOG_WARN(TAG_HTTP, "Connect failed for /complete");
        }

        if (done_ok)
        {
            uint32_t upload_ms = HAL_GetTick() - upload_start_tick;
            uint32_t kbps = (upload_ms > 0) ? (data_len / upload_ms) : 0;
            LOG_INFO(TAG_HTTP, "[PERF] Chunked upload: %lu bytes in %lums (%lu KB/s), task %lu",
                     (unsigned long)data_len, (unsigned long)upload_ms,
                     (unsigned long)kbps, (unsigned long)resolved_task_id);
            BSP_LED_Off(LED_GREEN);
            _powersave_restore(ps_prev);
            return WIFI_OK;
        }

        complete_failures++;
        if (complete_failures > UPLOAD_CHUNK_MAX_RETRIES)
        {
            /* Every byte IS on the server at this point — only the final
             * ack was lost. Report failure so the caller's own retry logic
             * runs, but this is a much narrower failure mode than losing
             * the whole frame: a retried /complete against already-
             * up-to-date sidecar state just re-validates and re-processes. */
            LOG_ERROR(TAG_HTTP, "Upload /complete failed %lu times — bytes are on the server "
                      "but finalization was never confirmed", (unsigned long)complete_failures);
            BSP_LED_Off(LED_GREEN);
            _powersave_restore(ps_prev);
            return WIFI_ERROR_SEND;
        }
        HAL_Delay(complete_backoff_ms);
        complete_backoff_ms = (complete_backoff_ms * 2 > UPLOAD_CHUNK_RETRY_MAX_MS)
                              ? UPLOAD_CHUNK_RETRY_MAX_MS : complete_backoff_ms * 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP GET — Server Time Synchronization
 *
 *  Fetches GET /api/time and parses the JSON response:
 *    {"hour":14,"minute":32,"second":10,"year":26,"month":3,"day":8,"weekday":6}
 * ═══════════════════════════════════════════════════════════════════════════ */

WiFiStatus_t WiFi_HttpGetTime(uint8_t *hour, uint8_t *minute, uint8_t *second,
                               uint8_t *year, uint8_t *month, uint8_t *day,
                               uint8_t *weekday)
{
    if (!s_connected)
    {
        LOG_ERROR(TAG_HTTP, "Cannot GET time — Wi-Fi not connected");
        return WIFI_ERROR_SEND;
    }

    LOG_INFO(TAG_HTTP, "Fetching server time from %s:%d%s...",
             SERVER_HOST, SERVER_PORT, SERVER_TIME_PATH);

    /* ── 1. Build HTTP GET request ─────────────────────── */

    int header_len = snprintf(s_http_header, HTTP_HEADER_MAX,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_TIME_PATH,
        SERVER_HOST, SERVER_PORT);

    /* ── 2. Open TCP socket ────────────────────────────── */

    int32_t sock = MX_WIFI_Socket_create(wifi_obj_get(),
                                          MX_AF_INET, MX_SOCK_STREAM, MX_IPPROTO_TCP);
    if (sock < 0)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: socket create failed (err=%ld)", (long)sock);
        return WIFI_ERROR_SEND;
    }

    struct mx_sockaddr_in server_addr = {0};
    server_addr.sin_len    = (uint8_t)sizeof(server_addr);
    server_addr.sin_family = MX_AF_INET;
    server_addr.sin_port   = (uint16_t)((SERVER_PORT >> 8) | ((SERVER_PORT & 0xFF) << 8));
    server_addr.sin_addr.s_addr = (uint32_t)mx_aton_r(SERVER_HOST);

    int32_t ret = MX_WIFI_Socket_connect(
        wifi_obj_get(), sock,
        (struct mx_sockaddr *)&server_addr,
        (int32_t)sizeof(server_addr));

    if (ret < 0)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: connect to %s:%d failed (err=%ld)",
                  SERVER_HOST, SERVER_PORT, (long)ret);
        MX_WIFI_Socket_close(wifi_obj_get(), sock);
        return WIFI_ERROR_SEND;
    }

    /* ── 3. Send GET request ───────────────────────────── */

    if (_socket_send_all(sock, (uint8_t *)s_http_header, header_len) != 0)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: failed to send GET request");
        MX_WIFI_Socket_close(wifi_obj_get(), sock);
        return WIFI_ERROR_SEND;
    }

    /* ── 4. Read response (poll with small yields) ── */

    /* The EMW3080 processes HTTP responses asynchronously through SPI.
     * Poll continuously with small yields until the response arrives,
     * drastically reducing boot time compared to the legacy 2000ms hard delay. */
    uint8_t resp_buf[512] = {0};
    int32_t resp_len = 0;
    uint32_t start_tick = HAL_GetTick();

    while ((HAL_GetTick() - start_tick) < HTTP_RESPONSE_TIMEOUT_MS)
    {
        MX_WIFI_IO_YIELD(wifi_obj_get(), 50);
        resp_len = MX_WIFI_Socket_recv(
            wifi_obj_get(), sock, resp_buf, sizeof(resp_buf) - 1, 0);

        if (resp_len > 0)
        {
            break;
        }
    }

    MX_WIFI_Socket_close(wifi_obj_get(), sock);

    if (resp_len <= 0)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: no response from server");
        return WIFI_ERROR_TIMEOUT;
    }

    resp_buf[resp_len] = '\0';

    /* ── 5. Find JSON body ──────────────────────────────── */
    /* FastAPI/Uvicorn may use chunked transfer encoding, which adds
     * hex chunk-size lines before the JSON payload. We skip past the
     * HTTP headers (\r\n\r\n) and then find the first '{' character. */

    const char *hdr_end = strstr((char *)resp_buf, "\r\n\r\n");
    if (hdr_end == NULL)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: no HTTP body in response");
        LOG_DEBUG(TAG_HTTP, "Raw response: %.120s", (char *)resp_buf);
        return WIFI_ERROR_SEND;
    }

    const char *body = strchr(hdr_end + 4, '{');
    if (body == NULL)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: no JSON object in body");
        LOG_DEBUG(TAG_HTTP, "Body starts: %.80s", hdr_end + 4);
        return WIFI_ERROR_SEND;
    }

    LOG_DEBUG(TAG_HTTP, "Time sync response body: %.80s", body);

    /* ── 6. Parse JSON fields ──────────────────────────── */

    json_mem_reset();
    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: JSON parse failed");
        return WIFI_ERROR_SEND;
    }

    cJSON *j_hour    = cJSON_GetObjectItem(root, "hour");
    cJSON *j_minute  = cJSON_GetObjectItem(root, "minute");
    cJSON *j_second  = cJSON_GetObjectItem(root, "second");
    cJSON *j_year    = cJSON_GetObjectItem(root, "year");
    cJSON *j_month   = cJSON_GetObjectItem(root, "month");
    cJSON *j_day     = cJSON_GetObjectItem(root, "day");
    cJSON *j_weekday = cJSON_GetObjectItem(root, "weekday");

    if (!cJSON_IsNumber(j_hour) || !cJSON_IsNumber(j_minute) || !cJSON_IsNumber(j_second))
    {
        LOG_ERROR(TAG_HTTP, "Time sync: missing time fields in JSON");
        cJSON_Delete(root);
        return WIFI_ERROR_SEND;
    }

    /* ── SEC-04: Range-validate all fields before writing to RTC ── */
    int raw_h = j_hour->valueint;
    int raw_m = j_minute->valueint;
    int raw_s = j_second->valueint;
    int raw_y = cJSON_IsNumber(j_year)    ? j_year->valueint    : 26;
    int raw_mo = cJSON_IsNumber(j_month)  ? j_month->valueint   : 1;
    int raw_d = cJSON_IsNumber(j_day)     ? j_day->valueint     : 1;
    int raw_wd = cJSON_IsNumber(j_weekday)? j_weekday->valueint : 1;

    if (raw_h < 0 || raw_h > 23 || raw_m < 0 || raw_m > 59 ||
        raw_s < 0 || raw_s > 59 || raw_mo < 1 || raw_mo > 12 ||
        raw_d < 1 || raw_d > 31 || raw_wd < 1 || raw_wd > 7 ||
        raw_y < 0 || raw_y > 99)
    {
        LOG_ERROR(TAG_HTTP, "Time sync: field out of range (h=%d m=%d s=%d)",
                  raw_h, raw_m, raw_s);
        cJSON_Delete(root);
        return WIFI_ERROR_SEND;
    }

    *hour    = (uint8_t)raw_h;
    *minute  = (uint8_t)raw_m;
    *second  = (uint8_t)raw_s;
    *year    = (uint8_t)raw_y;
    *month   = (uint8_t)raw_mo;
    *day     = (uint8_t)raw_d;
    *weekday = (uint8_t)raw_wd;

    LOG_INFO(TAG_HTTP, "Server time: %02u:%02u:%02u  %04u-%02u-%02u (wd=%u)",
             *hour, *minute, *second, 2000 + *year, *month, *day, *weekday);

    cJSON_Delete(root);
    return WIFI_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Shared TCP Helper (ARCH-02)
 * ═══════════════════════════════════════════════════════════════════════════ */

int32_t WiFi_TcpConnect(const char *host, uint16_t port)
{
    if (!s_connected) return -1;

    int32_t sock = MX_WIFI_Socket_create(
        wifi_obj_get(), MX_AF_INET, MX_SOCK_STREAM, MX_IPPROTO_TCP);
    if (sock < 0) return -1;

    struct mx_sockaddr_in addr = {0};
    addr.sin_len    = (uint8_t)sizeof(addr);
    addr.sin_family = MX_AF_INET;
    addr.sin_port   = (uint16_t)((port >> 8) | ((port & 0xFF) << 8));
    addr.sin_addr.s_addr = (uint32_t)mx_aton_r(host);

    int32_t ret = MX_WIFI_Socket_connect(
        wifi_obj_get(), sock,
        (struct mx_sockaddr *)&addr, (int32_t)sizeof(addr));

    if (ret < 0)
    {
        MX_WIFI_Socket_close(wifi_obj_get(), sock);
        return -1;
    }

    /* CRITICAL: Set a sane hardware receive and send timeout (4s) so the EMW3080
     * doesn't block the MIPC layer up to 30s (MX_WIFI_CMD_TIMEOUT) and trip the 16s watchdog!
     * NOTE: MX_WIFI_Socket_setsockopt explicitly expects a 4-byte int32_t representing ms.
     * Passing an 8-byte POSIX timeval struct corrupts the AT firmware's timeout state. */
    int32_t mx_timeout = 4000;
    MX_WIFI_Socket_setsockopt(wifi_obj_get(), sock, MX_SOL_SOCKET, MX_SO_RCVTIMEO, &mx_timeout, sizeof(mx_timeout));
    MX_WIFI_Socket_setsockopt(wifi_obj_get(), sock, MX_SOL_SOCKET, MX_SO_SNDTIMEO, &mx_timeout, sizeof(mx_timeout));

    return sock;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Shutdown
 * ═══════════════════════════════════════════════════════════════════════════ */

void WiFi_DeInit(void)
{
    LOG_INFO(TAG_WIFI, "Shutting down Wi-Fi module");

    if (s_connected)
    {
        MX_WIFI_Disconnect(wifi_obj_get());
        s_connected = 0;
    }

    if (s_initialized)
    {
        MX_WIFI_DeInit(wifi_obj_get());
        s_initialized = 0;
    }
}
