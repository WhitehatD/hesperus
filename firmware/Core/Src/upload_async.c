/**
 * @file    upload_async.c
 * @brief   Non-blocking HTTP Image Upload — Cooperative State Machine
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * STALE — DO NOT WIRE IN AS-IS (2026-08-23). This implements a single-POST
 * multipart/form-data upload against `SERVER_UPLOAD_PATH?task_id=N`. The
 * live protocol in wifi.c (WiFi_HttpPostImage) has since moved to a
 * chunked-POST + `/complete` design with CRC32, a resume probe
 * (`GET /api/upload/resume`), and stall-detection/keep-alive-socket tuning
 * that took three separate hardware-debugging sessions to get right on a
 * real 4G hotspot. Wiring this file in unmodified would silently regress
 * all of that. The main-loop-blocks-during-upload problem this file was
 * meant to solve is instead being closed incrementally inside
 * _socket_send_all()/WiFi_HttpPostImage() in wifi.c (periodic
 * MQTT_ProcessLoop() servicing during backpressure stalls) — see the
 * 2026-08-23 comment there. If this file is ever revived, it must first be
 * re-ported onto wifi.c's current wire protocol, then hardware-validated
 * exactly like every other change to this upload path.
 *
 * Each call to Upload_Poll() advances the state machine by sending
 * up to one chunk of data (~16KB). The main loop remains responsive
 * for MQTT, watchdog, and new commands between polls.
 *
 * Wire format (same as WiFi_HttpPostImage):
 *   POST /api/upload?task_id=N HTTP/1.1\r\n
 *   Host: <server>:<port>\r\n
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

#include "upload_async.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "wifi.h"
#include "main.h"

#include "mx_wifi.h"
#include "mx_wifi_io.h"

#include <string.h>
#include <stdio.h>

/* ── Configuration ─────────────────────────────────── */

/** Max bytes to send per Upload_Poll() call.
 *  16KB balances throughput vs responsiveness:
 *  - At ~1 Mbps SPI, 16KB takes ~130ms to transmit
 *  - Main loop gets control back every ~130ms for MQTT/watchdog */
#define UPLOAD_CHUNK_PER_POLL   HTTP_UPLOAD_CHUNK_SIZE

/** Upload timeout — abort if no progress for this long */
#define UPLOAD_TIMEOUT_MS       30000

/** MIME boundary (must match server expectations) */
#define MULTIPART_BOUNDARY      "----ThesisIoTBoundary2026"

/* ── Internal Helpers ──────────────────────────────── */

/**
 * @brief  Send up to `max_bytes` from a buffer.
 *         Returns number of bytes actually sent (may be less than max_bytes).
 *
 * NOTE: MX_WIFI_Socket_send's last parameter is declared as `flags` in the
 * header but the ST MIPC implementation uses it as a send timeout in ms.
 * Passing 0 causes immediate return when the SPI buffer is full (= no progress).
 * We use a short timeout (200ms) to allow the EMW3080 to drain its TX buffer
 * while keeping each call bounded for cooperative scheduling.
 */
static int32_t _send_partial(int32_t sock, const uint8_t *buf, int32_t len, int32_t max_bytes)
{
    int32_t to_send = (len < max_bytes) ? len : max_bytes;
    if (to_send <= 0) return 0;

    int32_t sent = MX_WIFI_Socket_send(
        wifi_obj_get(), sock,
        (uint8_t *)buf, to_send,
        HTTP_RESPONSE_TIMEOUT_MS /* timeout_ms — NOT flags; see note above */);

    if (sent > 0)
    {
        MX_WIFI_IO_YIELD(wifi_obj_get(), 2);
        return sent;
    }

    MX_WIFI_IO_YIELD(wifi_obj_get(), 10);
    return 0;
}

/* ── Public API ────────────────────────────────────── */

int Upload_Start(UploadCtx_t *ctx, uint32_t task_id,
                 uint8_t *data, uint32_t data_len)
{
    if (ctx->state != UPLOAD_IDLE &&
        ctx->state != UPLOAD_COMPLETE &&
        ctx->state != UPLOAD_FAILED)
    {
        LOG_WARN(TAG_HTTP, "Upload_Start rejected — upload already in progress");
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->state     = UPLOAD_CONNECTING;
    ctx->sock      = -1;
    ctx->task_id   = task_id;
    ctx->data      = data;
    ctx->data_len  = data_len;
    ctx->start_tick = HAL_GetTick();

    /* Pre-build the multipart segments */
    ctx->part_header_len = snprintf(ctx->part_header, sizeof(ctx->part_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"capture_%lu.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n",
        MULTIPART_BOUNDARY, (unsigned long)task_id);

    ctx->part_footer_len = snprintf(ctx->part_footer, sizeof(ctx->part_footer),
        "\r\n--%s--\r\n", MULTIPART_BOUNDARY);

    uint32_t body_length = (uint32_t)ctx->part_header_len
                         + data_len
                         + (uint32_t)ctx->part_footer_len;

    ctx->http_header_len = snprintf(ctx->http_header, sizeof(ctx->http_header),
        "POST %s?task_id=%lu HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Upload-Token: %s\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        SERVER_UPLOAD_PATH, (unsigned long)task_id,
        SERVER_HOST, SERVER_PORT,
        FIRMWARE_UPLOAD_TOKEN,
        MULTIPART_BOUNDARY,
        (unsigned long)body_length);

    LOG_INFO(TAG_HTTP, "Async upload started: task=%lu, %lu bytes",
             (unsigned long)task_id, (unsigned long)data_len);

    return 0;
}


UploadState_t Upload_Poll(UploadCtx_t *ctx)
{
    if (ctx->state == UPLOAD_IDLE ||
        ctx->state == UPLOAD_COMPLETE ||
        ctx->state == UPLOAD_FAILED)
    {
        return ctx->state;
    }

    /* Global timeout check */
    if ((HAL_GetTick() - ctx->start_tick) > UPLOAD_TIMEOUT_MS)
    {
        LOG_ERROR(TAG_HTTP, "Async upload timeout after %lums",
                  (unsigned long)(HAL_GetTick() - ctx->start_tick));
        Upload_Abort(ctx);
        ctx->state = UPLOAD_FAILED;
        return UPLOAD_FAILED;
    }

#if WATCHDOG_ENABLED
    IWDG->KR = 0x0000AAAAu;  /* Refresh watchdog during long uploads */
#endif

    switch (ctx->state)
    {
    /* ── CONNECTING: Open TCP socket to server ─────── */
    case UPLOAD_CONNECTING:
    {
        ctx->sock = WiFi_TcpConnect(SERVER_HOST, SERVER_PORT);
        if (ctx->sock < 0)
        {
            ctx->retries++;
            if (ctx->retries >= HTTP_UPLOAD_MAX_RETRIES)
            {
                LOG_ERROR(TAG_HTTP, "Async upload: connect failed after %d retries",
                          ctx->retries);
                ctx->state = UPLOAD_FAILED;
                return UPLOAD_FAILED;
            }
            LOG_WARN(TAG_HTTP, "Connect failed, retry %d/%d",
                     ctx->retries, HTTP_UPLOAD_MAX_RETRIES);
            /* Will retry on next poll */
            return UPLOAD_CONNECTING;
        }

        LOG_DEBUG(TAG_HTTP, "Async: TCP connected to %s:%d", SERVER_HOST, SERVER_PORT);
        ctx->state = UPLOAD_SEND_HEADER;
        ctx->segment_offset = 0;
        return UPLOAD_SEND_HEADER;
    }

    /* ── SEND_HEADER: HTTP request headers + multipart header ─── */
    case UPLOAD_SEND_HEADER:
    {
        /* Send HTTP headers */
        int total_hdr = ctx->http_header_len + ctx->part_header_len;
        int remaining = total_hdr - ctx->segment_offset;

        if (remaining > 0)
        {
            /* Build combined header+part_header for single send phase */
            const uint8_t *src;
            int32_t chunk_len;

            if (ctx->segment_offset < ctx->http_header_len)
            {
                src = (const uint8_t *)ctx->http_header + ctx->segment_offset;
                chunk_len = ctx->http_header_len - ctx->segment_offset;
            }
            else
            {
                int part_off = ctx->segment_offset - ctx->http_header_len;
                src = (const uint8_t *)ctx->part_header + part_off;
                chunk_len = ctx->part_header_len - part_off;
            }

            int32_t sent = _send_partial(ctx->sock, src, chunk_len, UPLOAD_CHUNK_PER_POLL);
            if (sent < 0)
            {
                Upload_Abort(ctx);
                ctx->state = UPLOAD_FAILED;
                return UPLOAD_FAILED;
            }
            ctx->segment_offset += sent;
        }

        if (ctx->segment_offset >= total_hdr)
        {
            /* Headers fully sent — move to data phase */
            ctx->state = UPLOAD_SEND_DATA;
            ctx->offset = 0;
            LOG_DEBUG(TAG_HTTP, "Async: headers sent (%d bytes)", total_hdr);
        }
        return ctx->state;
    }

    /* ── SEND_DATA: Image payload — send as much as possible per poll ── */
    case UPLOAD_SEND_DATA:
    {
        /* Send multiple chunks per poll call to maximize throughput.
         * Each MX_WIFI_Socket_send takes ~2-10ms (SPI transfer).
         * Budget: up to 200ms of sends per poll, then yield. */
        uint32_t poll_start_tick = HAL_GetTick();
        int stall_count = 0;

        while (ctx->offset < ctx->data_len)
        {
#if WATCHDOG_ENABLED
            IWDG->KR = 0x0000AAAAu;
#endif
            int32_t remaining = (int32_t)(ctx->data_len - ctx->offset);
            int32_t sent = _send_partial(
                ctx->sock,
                ctx->data + ctx->offset,
                remaining,
                UPLOAD_CHUNK_PER_POLL);

            if (sent > 0)
            {
                ctx->offset += (uint32_t)sent;
                stall_count = 0;
                BSP_LED_Toggle(LED_GREEN);
            }
            else
            {
                stall_count++;
                if (stall_count > 50)  /* 50 * 10ms = 500ms stall → give up this poll */
                    break;
            }

            /* Yield back to main loop every 200ms for MQTT keepalive */
            if ((HAL_GetTick() - poll_start_tick) > 200)
                break;
        }

        if (ctx->offset >= ctx->data_len)
        {
            ctx->state = UPLOAD_SEND_FOOTER;
            ctx->segment_offset = 0;
            LOG_DEBUG(TAG_HTTP, "Async: image data sent (%lu bytes)",
                      (unsigned long)ctx->data_len);
        }
        return ctx->state;
    }

    /* ── SEND_FOOTER: Multipart closing boundary ───── */
    case UPLOAD_SEND_FOOTER:
    {
        int remaining = ctx->part_footer_len - ctx->segment_offset;
        if (remaining > 0)
        {
            int32_t sent = _send_partial(
                ctx->sock,
                (const uint8_t *)ctx->part_footer + ctx->segment_offset,
                remaining,
                UPLOAD_CHUNK_PER_POLL);

            if (sent < 0)
            {
                Upload_Abort(ctx);
                ctx->state = UPLOAD_FAILED;
                return UPLOAD_FAILED;
            }
            ctx->segment_offset += sent;
        }

        if (ctx->segment_offset >= ctx->part_footer_len)
        {
            ctx->state = UPLOAD_RECV_RESPONSE;
            ctx->segment_offset = 0;
        }
        return ctx->state;
    }

    /* ── RECV_RESPONSE: Read server's HTTP response ── */
    case UPLOAD_RECV_RESPONSE:
    {
        MX_WIFI_IO_YIELD(wifi_obj_get(), 50);

        uint8_t resp[256] = {0};
        int32_t n = MX_WIFI_Socket_recv(
            wifi_obj_get(), ctx->sock,
            resp, sizeof(resp) - 1, 0);

        if (n > 0)
        {
            resp[n] = '\0';

            /* Parse HTTP status code */
            int http_code = 0;
            const char *sp = strchr((char *)resp, ' ');
            if (sp)
            {
                http_code = atoi(sp + 1);
            }

            if (http_code >= 200 && http_code < 300)
            {
                uint32_t elapsed = HAL_GetTick() - ctx->start_tick;
                /* Tenths of KB/s — integer bytes/ms floors sub-1.0 KB/s to "0". */
                uint32_t kbps_x10 = elapsed > 0 ? (ctx->data_len * 10u / elapsed) : 0;
                LOG_INFO(TAG_HTTP, "Async upload OK (HTTP %d, %lu bytes, %lums, %lu.%lu KB/s)",
                         http_code,
                         (unsigned long)ctx->data_len,
                         (unsigned long)elapsed,
                         (unsigned long)(kbps_x10 / 10u),
                         (unsigned long)(kbps_x10 % 10u));
            }
            else
            {
                LOG_WARN(TAG_HTTP, "Server returned HTTP %d", http_code);
            }

            MX_WIFI_Socket_close(wifi_obj_get(), ctx->sock);
            ctx->sock = -1;
            ctx->state = UPLOAD_COMPLETE;
            return UPLOAD_COMPLETE;
        }

        /* No response yet — keep polling (timeout will catch hangs) */
        ctx->segment_offset++;
        if (ctx->segment_offset > 100)  /* ~5 seconds of polling at 50ms */
        {
            LOG_WARN(TAG_HTTP, "No server response — treating as success");
            MX_WIFI_Socket_close(wifi_obj_get(), ctx->sock);
            ctx->sock = -1;
            ctx->state = UPLOAD_COMPLETE;
            return UPLOAD_COMPLETE;
        }
        return UPLOAD_RECV_RESPONSE;
    }

    default:
        return ctx->state;
    }
}


int Upload_IsBusy(const UploadCtx_t *ctx)
{
    return (ctx->state != UPLOAD_IDLE &&
            ctx->state != UPLOAD_COMPLETE &&
            ctx->state != UPLOAD_FAILED);
}


void Upload_Abort(UploadCtx_t *ctx)
{
    if (ctx->sock >= 0)
    {
        MX_WIFI_Socket_close(wifi_obj_get(), ctx->sock);
        ctx->sock = -1;
    }
    ctx->state = UPLOAD_FAILED;
    LOG_WARN(TAG_HTTP, "Upload aborted (task=%lu)", (unsigned long)ctx->task_id);
}
