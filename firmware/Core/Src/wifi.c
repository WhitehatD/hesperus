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

/* Reusable TX buffer for HTTP requests (header + body boundaries) */
#define HTTP_HEADER_MAX  512
static char s_http_header[HTTP_HEADER_MAX];

/* MIME boundary for multipart upload */
#define MULTIPART_BOUNDARY  "----ThesisIoTBoundary2026"

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
 * @brief  Helper — send a buffer over a WiFi socket with retry on partial send.
 * @retval 0 on success, -1 on failure
 */
static int _socket_send_all(int32_t sock, const uint8_t *data, int32_t len)
{
    int32_t offset = 0;
    int retries = 0;
    uint32_t last_mqtt_tick = HAL_GetTick();

    while (offset < len)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu; /* Prevent 16s watchdog reset during 37s uploads */
#endif
        /* Send in chunks to prevent EMW3080 SPI buffer flooding.
         * Yield the SPI pipeline between chunks so the WiFi module
         * can drain its internal TX buffers. */
        int32_t chunk = len - offset;
        if (chunk > HTTP_UPLOAD_CHUNK_SIZE)
            chunk = HTTP_UPLOAD_CHUNK_SIZE;

        int32_t sent = MX_WIFI_Socket_send(
            wifi_obj_get(), sock,
            (uint8_t *)(data + offset), chunk,
            HTTP_RESPONSE_TIMEOUT_MS);

        if (sent > 0)
        {
            /* Toggle GREEN LED to indicate data transfer */
            BSP_LED_Toggle(LED_GREEN);

            offset += sent;
            retries = 0;        /* Reset retry counter on progress */

            /* Minimal SPI yield between chunks — 2ms is sufficient for
             * the EMW3080 to drain its TX buffer at SPI clock speeds. */
            if (offset < len)
            {
                MX_WIFI_IO_YIELD(wifi_obj_get(), 2);
            }

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
        else
        {
            retries++;
            if (retries >= 3)
            {
                LOG_ERROR(TAG_HTTP, "Send failed after %d retries at offset %ld/%ld",
                          retries, (long)offset, (long)len);
                return -1;
            }
            HAL_Delay(50);     /* Brief pause before retry */
        }
    }
    return 0;
}

/**
 * Build and send an HTTP POST multipart/form-data request containing
 * the captured image to the FastAPI server's /api/upload endpoint.
 *
 * Wire format:
 *   POST /api/upload?task_id=N HTTP/1.1\r\n
 *   Host: <server>\r\n
 *   Content-Type: multipart/form-data; boundary=<boundary>\r\n
 *   Content-Length: <total>\r\n
 *   \r\n
 *   --<boundary>\r\n
 *   Content-Disposition: form-data; name="file"; filename="capture.jpg"\r\n
 *   Content-Type: image/jpeg\r\n
 *   \r\n
 *   <binary image data>\r\n
 *   --<boundary>--\r\n
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

    /* ── Enterprise: retry loop around the full POST sequence ── */
    WiFiStatus_t final_status = WIFI_ERROR_SEND;
    uint32_t retry_delay = HTTP_UPLOAD_RETRY_DELAY_MS;

    for (int attempt = 0; attempt <= HTTP_UPLOAD_MAX_RETRIES; attempt++)
    {
        if (attempt > 0)
        {
            LOG_WARN(TAG_HTTP, "Retry %d/%d after %lums...",
                     attempt, HTTP_UPLOAD_MAX_RETRIES,
                     (unsigned long)retry_delay);
            HAL_Delay(retry_delay);
            retry_delay *= 2;  /* Exponential backoff */
        }

        LOG_INFO(TAG_HTTP, "Uploading image (%lu bytes) for task %u (attempt %d)...",
                 (unsigned long)data_len, task_id, attempt + 1);

        /* ── 1. Build multipart body parts ──────────────────── */

        char part_header[256];
        int part_header_len = snprintf(part_header, sizeof(part_header),
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"capture_%lu.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n"
            "\r\n",
            MULTIPART_BOUNDARY, (unsigned long)task_id);

        char part_footer[64];
        int part_footer_len = snprintf(part_footer, sizeof(part_footer),
            "\r\n--%s--\r\n", MULTIPART_BOUNDARY);

        uint32_t body_length = (uint32_t)part_header_len + data_len + (uint32_t)part_footer_len;

        /* ── 2. Build HTTP request header ───────────────────── */

        int header_len = snprintf(s_http_header, HTTP_HEADER_MAX,
            "POST %s?task_id=%lu HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: multipart/form-data; boundary=%s\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n",
            SERVER_UPLOAD_PATH, (unsigned long)task_id,
            SERVER_HOST, SERVER_PORT,
            MULTIPART_BOUNDARY,
            (unsigned long)body_length);

        /* ── 3. Open TCP socket ─────────────────────────────── */

        int32_t sock = WiFi_TcpConnect(SERVER_HOST, SERVER_PORT);
        if (sock < 0)
        {
            LOG_ERROR(TAG_HTTP, "Socket connect to %s:%d failed",
                      SERVER_HOST, SERVER_PORT);
            continue;  /* Retry */
        }

        LOG_DEBUG(TAG_HTTP, "TCP connected to %s:%d", SERVER_HOST, SERVER_PORT);

        /* ── 4. Send data: header → part_header → image → footer ── */

        WiFiStatus_t status = WIFI_OK;

        if (_socket_send_all(sock, (uint8_t *)s_http_header, header_len) != 0)
        {
            LOG_ERROR(TAG_HTTP, "Failed to send HTTP header");
            status = WIFI_ERROR_SEND;
        }

        if (status == WIFI_OK &&
            _socket_send_all(sock, (uint8_t *)part_header, part_header_len) != 0)
        {
            LOG_ERROR(TAG_HTTP, "Failed to send part header");
            status = WIFI_ERROR_SEND;
        }

        if (status == WIFI_OK &&
            _socket_send_all(sock, data, (int32_t)data_len) != 0)
        {
            LOG_ERROR(TAG_HTTP, "Failed to send image data");
            status = WIFI_ERROR_SEND;
        }

        if (status == WIFI_OK &&
            _socket_send_all(sock, (uint8_t *)part_footer, part_footer_len) != 0)
        {
            LOG_ERROR(TAG_HTTP, "Failed to send part footer");
            status = WIFI_ERROR_SEND;
        }

        /* ── 5. Read response ──────────────────────────────── */

        if (status == WIFI_OK)
        {
            uint8_t resp_buf[256] = {0};
            int32_t resp_len = MX_WIFI_Socket_recv(
                wifi_obj_get(), sock, resp_buf, sizeof(resp_buf) - 1, 0);

            if (resp_len > 0)
            {
                resp_buf[resp_len] = '\0';
                int http_code = 0;
                const char *space = strchr((char *)resp_buf, ' ');
                if (space != NULL)
                {
                    char *endptr = NULL;
                    long parsed = strtol(space + 1, &endptr, 10);
                    if (endptr != space + 1 && parsed >= 100 && parsed <= 599)
                        http_code = (int)parsed;
                }

                if (http_code >= 200 && http_code < 300)
                {
                    LOG_INFO(TAG_HTTP, "Upload successful (HTTP %d)", http_code);
                }
                else
                {
                    LOG_WARN(TAG_HTTP, "Server returned HTTP %d: %.80s",
                             http_code, (char *)resp_buf);
                }
            }
            else
            {
                LOG_WARN(TAG_HTTP, "No response from server (timeout or closed)");
            }
        }

        MX_WIFI_Socket_close(wifi_obj_get(), sock);

        if (status == WIFI_OK)
        {
            /* ── PERF: Log upload throughput ── */
            uint32_t upload_ms = HAL_GetTick() - upload_start_tick;
            uint32_t kbps = (upload_ms > 0) ? (data_len / upload_ms) : 0;
            (void)kbps;
            LOG_INFO(TAG_HTTP, "[PERF] Upload: %lu bytes in %lums (%lu KB/s)",
                     (unsigned long)data_len, (unsigned long)upload_ms,
                     (unsigned long)kbps);

            final_status = WIFI_OK;
            break;  /* Success — no more retries */
        }

        /* Send failed — will retry if attempts remain */
    }

    BSP_LED_Off(LED_GREEN);
    return final_status;
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
