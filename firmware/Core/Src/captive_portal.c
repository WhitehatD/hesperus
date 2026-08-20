/**
 * @file    captive_portal.c
 * @brief   WiFi Captive Portal — SoftAP + Embedded HTTP Server + DNS Redirect
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Enterprise-grade captive portal for the B-U585I-IOT02A Discovery Kit.
 *
 * Architecture:
 *   1. Start SoftAP (MX_WIFI_StartAP) — creates "IoT-Setup-XXXX" network
 *   2. DNS Redirect (UDP port 53) — all queries resolve to 192.168.10.1
 *      → triggers automatic captive portal detection on iOS/Android/Windows
 *   3. HTTP Server (TCP port 80) — serves configuration page and handles form POST
 *   4. Flash Save (WiFiCred_Save) — persists credentials across power cycles
 *   5. System Reboot (NVIC_SystemReset) — reconnects with new credentials
 *
 * The DNS redirect is critical for mobile UX: without it, phones show
 * "Internet may not be available" but don't auto-open the portal page.
 */

#include "captive_portal.h"
#include "wifi_credentials.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "wifi.h"
#include "board_status.h"
#include "main.h"

#include "mx_wifi.h"
#include "mx_wifi_io.h"
#include "mx_address.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Module State
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint8_t s_portal_active = 0;
static int32_t s_http_server_sock = -1;
static int32_t s_dns_sock = -1;
static PortalReason_t s_portal_reason = PORTAL_REASON_FRESH;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Embedded HTML — Configuration Page
 *
 *  Premium, responsive design with dark theme, glass-morphism, and
 *  smooth animations. Inline CSS — no external dependencies.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char HTML_CONFIG_BODY[] =
    "<!DOCTYPE html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IoT Sensor - WiFi Setup</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{"
        "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "min-height:100vh;"
        "display:flex;align-items:center;justify-content:center;"
        "background:linear-gradient(135deg,#0a0e27 0%,#1a1f3a 50%,#0d1117 100%);"
        "color:#e6edf3;padding:20px"
    "}"
    ".card{"
        "background:rgba(22,27,52,0.85);"
        "backdrop-filter:blur(20px);"
        "-webkit-backdrop-filter:blur(20px);"
        "border:1px solid rgba(99,140,255,0.15);"
        "border-radius:20px;"
        "padding:40px 36px;"
        "width:100%;max-width:420px;"
        "box-shadow:0 8px 40px rgba(0,0,0,0.4),0 0 80px rgba(99,140,255,0.06);"
        "animation:fadeUp 0.6s ease-out"
    "}"
    "@keyframes fadeUp{from{opacity:0;transform:translateY(20px)}to{opacity:1;transform:none}}"
    ".logo{"
        "width:56px;height:56px;"
        "background:linear-gradient(135deg,#638cff,#4f6ef7);"
        "border-radius:14px;"
        "display:flex;align-items:center;justify-content:center;"
        "margin:0 auto 20px;"
        "box-shadow:0 4px 20px rgba(99,140,255,0.3)"
    "}"
    ".logo svg{width:28px;height:28px;fill:white}"
    "h1{font-size:22px;font-weight:700;text-align:center;margin-bottom:6px;"
        "background:linear-gradient(135deg,#fff,#a8c0ff);-webkit-background-clip:text;"
        "-webkit-text-fill-color:transparent}"
    ".sub{text-align:center;color:#8b949e;font-size:13px;margin-bottom:28px}"
    "label{display:block;font-size:13px;font-weight:600;color:#a8b2c1;"
        "margin-bottom:6px;letter-spacing:0.3px}"
    ".field{margin-bottom:20px}"
    "input[type=text],input[type=password]{"
        "width:100%;padding:12px 14px;font-size:15px;"
        "background:rgba(13,17,23,0.6);"
        "border:1.5px solid rgba(99,140,255,0.2);"
        "border-radius:10px;color:#e6edf3;"
        "transition:border-color 0.2s,box-shadow 0.2s;outline:none"
    "}"
    "input:focus{"
        "border-color:#638cff;"
        "box-shadow:0 0 0 3px rgba(99,140,255,0.15)"
    "}"
    "input::placeholder{color:#484f58}"
    ".toggle{"
        "display:flex;align-items:center;gap:8px;"
        "font-size:12px;color:#8b949e;cursor:pointer;"
        "margin-top:6px;user-select:none"
    "}"
    ".toggle input{width:14px;height:14px;accent-color:#638cff}"
    "button{"
        "width:100%;padding:13px;font-size:15px;font-weight:600;"
        "background:linear-gradient(135deg,#638cff,#4f6ef7);"
        "color:#fff;border:none;border-radius:10px;cursor:pointer;"
        "transition:transform 0.15s,box-shadow 0.15s;"
        "box-shadow:0 4px 16px rgba(99,140,255,0.3);margin-top:8px"
    "}"
    "button:hover{transform:translateY(-1px);box-shadow:0 6px 24px rgba(99,140,255,0.4)}"
    "button:active{transform:translateY(0)}"
    ".info{"
        "margin-top:24px;padding:14px;font-size:12px;line-height:1.5;"
        "background:rgba(99,140,255,0.06);border:1px solid rgba(99,140,255,0.1);"
        "border-radius:10px;color:#8b949e;text-align:center"
    "}"
    ".fw{text-align:center;margin-top:16px;font-size:11px;color:#484f58}"
    "</style>"
    "</head>"
    "<body onpageshow=\"var b=document.getElementById('btn');if(b){b.innerText='Save &amp; Connect';b.style.opacity='1';b.style.pointerEvents='auto';}\">"
    "<div class='card'>"
    "<div class='logo'>"
    "<svg viewBox='0 0 24 24'><path d='M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 "
    "10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21"
    ".21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3"
    "c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 "
    "4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z'/></svg>"
    "</div>"
    "<h1>WiFi Configuration</h1>"
    "<p class='sub'>IoT Visual Monitoring Sensor</p>"
    "<form method='POST' action='/configure' onsubmit=\"var b=document.getElementById('btn');b.innerText='Connecting...';b.style.opacity='0.8';b.style.pointerEvents='none';\">"
    "<div class='field'>"
    "<label for='ssid'>Network Name (SSID)</label>"
    "<input type='text' id='ssid' name='ssid' placeholder='Enter WiFi name' required maxlength='32' autocomplete='off'>"
    "</div>"
    "<div class='field'>"
    "<label for='password'>Password</label>"
    "<input type='password' id='password' name='password' placeholder='Enter WiFi password' required maxlength='63'>"
    "<label class='toggle'><input type='checkbox' onclick=\"var p=document.getElementById('password');"
    "p.type=p.type==='password'?'text':'password'\">Show password</label>"
    "</div>"
    "<button id='btn' type='submit'>Save &amp; Connect</button>"
    "</form>"
    "<div class='info'>"
    "The sensor will reboot and connect to your WiFi network. "
    "If connection fails, this setup page will reappear."
    "</div>"
    "<p class='fw'>Firmware v" FW_VERSION "</p>"
    "</div>"
    "</body></html>";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Success Response Page
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char HTML_SUCCESS_BODY[] =
    "<!DOCTYPE html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Configured</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{"
        "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "min-height:100vh;display:flex;align-items:center;justify-content:center;"
        "background:linear-gradient(135deg,#0a0e27,#1a1f3a,#0d1117);color:#e6edf3;padding:20px"
    "}"
    ".card{"
        "background:rgba(22,27,52,0.85);backdrop-filter:blur(20px);"
        "border:1px solid rgba(52,211,153,0.2);border-radius:20px;"
        "padding:40px 36px;width:100%;max-width:420px;text-align:center;"
        "animation:fadeUp 0.6s ease-out"
    "}"
    "@keyframes fadeUp{from{opacity:0;transform:translateY(20px)}to{opacity:1;transform:none}}"
    ".check{"
        "width:64px;height:64px;margin:0 auto 20px;"
        "background:linear-gradient(135deg,#34d399,#10b981);"
        "border-radius:50%;display:flex;align-items:center;justify-content:center;"
        "box-shadow:0 4px 20px rgba(52,211,153,0.3);animation:pulse 2s infinite"
    "}"
    "@keyframes pulse{0%,100%{box-shadow:0 4px 20px rgba(52,211,153,0.3)}"
        "50%{box-shadow:0 4px 30px rgba(52,211,153,0.5)}}"
    ".check svg{width:32px;height:32px;fill:white}"
    "h1{font-size:22px;font-weight:700;margin-bottom:8px;"
        "background:linear-gradient(135deg,#fff,#a8ffc8);-webkit-background-clip:text;"
        "-webkit-text-fill-color:transparent}"
    "p{color:#8b949e;font-size:14px;line-height:1.6;margin-bottom:8px}"
    ".countdown{color:#34d399;font-weight:600;font-size:15px;margin-top:16px}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<div class='check'>"
    "<svg viewBox='0 0 24 24'><path d='M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z'/></svg>"
    "</div>"
    "<h1>WiFi Configured!</h1>"
    "<p>Credentials saved. The sensor is rebooting to connect to your network.</p>"
    "<p class='countdown'>Rebooting in 3 seconds...</p>"
    "</div>"
    "</body></html>";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Error Page — Validation Failed
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char HTML_ERROR_BODY[] =
    "<!DOCTYPE html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Connection Failed</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "background:linear-gradient(135deg,#0a0e27,#1a1f3a,#0d1117);color:#e6edf3;padding:20px}"
    ".card{background:rgba(22,27,52,0.85);backdrop-filter:blur(20px);"
    "border:1px solid rgba(255,85,85,0.2);border-radius:20px;"
    "padding:40px 36px;width:100%;max-width:420px;text-align:center;"
    "animation:fadeUp 0.6s ease-out}"
    "@keyframes fadeUp{from{opacity:0;transform:translateY(20px)}to{opacity:1;transform:none}}"
    ".icon{width:64px;height:64px;margin:0 auto 20px;"
    "background:linear-gradient(135deg,#ff5555,#ff3333);"
    "border-radius:50%;display:flex;align-items:center;justify-content:center;"
    "box-shadow:0 4px 20px rgba(255,85,85,0.3)}"
    ".icon svg{width:32px;height:32px;fill:white}"
    "h1{font-size:22px;font-weight:700;margin-bottom:8px;"
    "background:linear-gradient(135deg,#fff,#ffcccc);-webkit-background-clip:text;"
    "-webkit-text-fill-color:transparent}"
    "p{color:#8b949e;font-size:14px;line-height:1.6;margin-bottom:24px}"
    "button{width:100%;padding:13px;font-size:15px;font-weight:600;"
    "background:linear-gradient(135deg,#638cff,#4f6ef7);color:#fff;border:none;border-radius:10px;cursor:pointer;"
    "transition:transform 0.15s,box-shadow 0.15s;box-shadow:0 4px 16px rgba(99,140,255,0.3)}"
    "button:hover{transform:translateY(-1px);box-shadow:0 6px 24px rgba(99,140,255,0.4)}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='icon'>"
    "<svg viewBox='0 0 24 24'><path d='M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z'/></svg>"
    "</div>"
    "<h1>Connection Failed</h1>"
    "<p>Could not connect to the WiFi network. Please check the spelling and password.</p>"
    "<button onclick='window.history.back()'>Try Again</button>"
    "</div></body></html>";

/* ═══════════════════════════════════════════════════════════════════════════
 *  404 Response
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char HTTP_REDIRECT_RESPONSE[] =
    "HTTP/1.1 302 Found\r\n"
    "Location: http://192.168.10.1/\r\n"
    "Connection: close\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP Request Parsing Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  URL-decode a string in-place.
 *         Handles %XX hex escapes and '+' → space.
 */
static void _url_decode(char *dst, const char *src, uint32_t max_len)
{
    uint32_t i = 0;
    while (*src && i < max_len - 1)
    {
        if (*src == '%' && src[1] && src[2])
        {
            /* Decode %XX hex pair */
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/**
 * @brief  Extract a named field from URL-encoded form data.
 *         E.g. from "ssid=MyNet&password=secret" extract "ssid" → "MyNet"
 */
static int _extract_form_field(const char *body, const char *field_name,
                                char *out, uint32_t out_size)
{
    /* Search for "field_name=" */
    char search[48];
    int slen = snprintf(search, sizeof(search), "%s=", field_name);
    if (slen <= 0) return -1;

    const char *start = strstr(body, search);
    if (start == NULL)
    {
        /* Try at start of body */
        if (strncmp(body, search, (size_t)slen) != 0)
            return -1;
        start = body;
    }

    start += slen;

    /* Find end of value: '&' or end of string */
    const char *end = strchr(start, '&');
    uint32_t raw_len = end ? (uint32_t)(end - start) : (uint32_t)strlen(start);

    /* Trim trailing whitespace/newlines */
    while (raw_len > 0 && (start[raw_len - 1] == '\r' ||
                            start[raw_len - 1] == '\n' ||
                            start[raw_len - 1] == ' '))
    {
        raw_len--;
    }

    if (raw_len == 0 || raw_len >= out_size * 3) /* URL-encoded can be 3× */
        return -1;

    /* Copy raw value temporarily */
    char raw[256];
    if (raw_len >= sizeof(raw)) raw_len = sizeof(raw) - 1;
    memcpy(raw, start, raw_len);
    raw[raw_len] = '\0';

    /* URL-decode */
    _url_decode(out, raw, out_size);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Socket Helper — send full buffer with retry
 * ═══════════════════════════════════════════════════════════════════════════ */

static int _portal_send_all(int32_t sock, const uint8_t *data, int32_t len)
{
    int32_t offset = 0;
    int retries = 0;

    while (offset < len)
    {
        int32_t chunk = len - offset;
        if (chunk > 1024) chunk = 1024;  /* Small chunks for portal */

        int32_t sent = MX_WIFI_Socket_send(
            wifi_obj_get(), sock,
            (uint8_t *)(data + offset), chunk, 0);

        if (sent > 0)
        {
            offset += sent;
            retries = 0;
            if (offset < len)
                MX_WIFI_IO_YIELD(wifi_obj_get(), 5);
        }
        else
        {
            retries++;
            /* Give TCP ACKs time to arrive. Apple Delayed ACK is 200ms.
             * 100 retries * 50ms = 5000ms maximum wait for socket buffer to drain. */
            if (retries >= 100) return -1;
            MX_WIFI_IO_YIELD(wifi_obj_get(), 50);
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DNS Redirect — Intercept all DNS queries, respond with our IP
 *
 *  Phones/laptops send DNS queries for captive portal detection URLs
 *  (e.g. connectivitycheck.gstatic.com, captive.apple.com).
 *  We respond to ALL queries with 192.168.10.1, triggering the
 *  automatic "Sign in to WiFi" popup.
 *
 *  DNS response format:
 *    [Transaction ID: 2B] [Flags: 2B] [QD: 2B] [AN: 2B]
 *    [NS: 2B] [AR: 2B] [Question section: copy] [Answer: A record]
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _dns_process_query(void)
{
    if (s_dns_sock < 0) return;

    uint8_t dns_buf[256];
    struct mx_sockaddr from_addr;
    uint32_t from_len = sizeof(from_addr);

    int32_t recv_len = MX_WIFI_Socket_recvfrom(
        wifi_obj_get(), s_dns_sock,
        dns_buf, sizeof(dns_buf), 0,
        &from_addr, &from_len);

    if (recv_len < 12) return;  /* Too short for DNS header */

    /* Build response:
     * Copy transaction ID from query, set response flags,
     * copy question section, append A record pointing to 192.168.10.1 */

    uint8_t resp[512];
    uint32_t resp_len = 0;

    /* Transaction ID (from query) */
    resp[resp_len++] = dns_buf[0];
    resp[resp_len++] = dns_buf[1];

    /* Flags: Standard response, Authoritative, No error */
    resp[resp_len++] = 0x81;  /* QR=1, Opcode=0, AA=1 */
    resp[resp_len++] = 0x80;  /* RA=1, RCODE=0 */

    /* Question count = 1 */
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x01;

    /* Answer count = 1 */
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x01;

    /* Authority + Additional = 0 */
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;

    /* Copy question section from query (skip 12-byte header) */
    uint32_t q_start = 12;
    uint32_t q_end = q_start;

    /* Parse question: labels + QTYPE(2) + QCLASS(2) */
    while (q_end < (uint32_t)recv_len && dns_buf[q_end] != 0)
    {
        q_end += dns_buf[q_end] + 1;  /* Skip label */
    }
    q_end++;  /* Skip null terminator */
    q_end += 4;  /* QTYPE + QCLASS */

    if (q_end > (uint32_t)recv_len || (resp_len + (q_end - q_start)) > sizeof(resp) - 20)
        return;  /* Malformed */

    memcpy(&resp[resp_len], &dns_buf[q_start], q_end - q_start);
    resp_len += (q_end - q_start);

    /* Answer resource record:
     *   Name: pointer to question (0xC00C)
     *   Type: A (0x0001)
     *   Class: IN (0x0001)
     *   TTL: 60 seconds
     *   RDLength: 4 (IPv4)
     *   RData: 192.168.10.1 */
    resp[resp_len++] = 0xC0;  /* Name pointer */
    resp[resp_len++] = 0x0C;  /* → offset 12 (question name) */
    resp[resp_len++] = 0x00;  /* Type: A */
    resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00;  /* Class: IN */
    resp[resp_len++] = 0x01;
    resp[resp_len++] = 0x00;  /* TTL: 60s */
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x00;
    resp[resp_len++] = 0x3C;
    resp[resp_len++] = 0x00;  /* RDLength: 4 */
    resp[resp_len++] = 0x04;
    resp[resp_len++] = 192;   /* 192.168.10.1 */
    resp[resp_len++] = 168;
    resp[resp_len++] = 10;
    resp[resp_len++] = 1;

    MX_WIFI_Socket_sendto(
        wifi_obj_get(), s_dns_sock,
        resp, (int32_t)resp_len, 0,
        &from_addr, (int32_t)from_len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP Request Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Send HTTP headers with dynamic Content-Length, followed by the body.
 */
static void _send_html_page(int32_t sock, const char *body)
{
    char headers[256];
    snprintf(headers, sizeof(headers),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store, no-cache, must-revalidate\r\n"
             "Content-Length: %d\r\n"
             "\r\n",
             (int)strlen(body));

    _portal_send_all(sock, (const uint8_t *)headers, (int32_t)strlen(headers));
    _portal_send_all(sock, (const uint8_t *)body, (int32_t)strlen(body));
}

/**
 * @brief  Handle a single HTTP client connection.
 * @retval 1 if credentials were submitted (reboot needed), 0 otherwise.
 */
static int _handle_http_client(int32_t client_sock)
{
    /* Read HTTP request with polling. 
     * IMPORTANT: Max 500ms timeout! Mobile browsers open 4-5 speculative TCP 
     * connections. If we block for 3000ms on an idle socket, the legitimate 
     * GET request socket gets backlogged and the browser signals a timeout! */
    uint8_t req_buf[1024];
    memset(req_buf, 0, sizeof(req_buf));

    int32_t total_recv = 0;
    uint32_t start = HAL_GetTick();
    uint32_t max_wait = 1200; /* Drop speculative connections after 1200ms */

    while ((HAL_GetTick() - start) < max_wait && total_recv < (int32_t)(sizeof(req_buf) - 1))
    {
        MX_WIFI_IO_YIELD(wifi_obj_get(), 10);

        /* Block for max 150ms per iteration at MIPC hardware layer */
        int32_t n = MX_WIFI_Socket_recv_timeout(
            wifi_obj_get(), client_sock,
            &req_buf[total_recv],
            (int32_t)(sizeof(req_buf) - 1 - (uint32_t)total_recv), 0,
            150);

        if (n > 0)
        {
            /* Legitimate data received! Extend window for slow POST payloads */
            max_wait = 3000;
            start = HAL_GetTick(); /* Reset timer */
            
            total_recv += n;
            req_buf[total_recv] = '\0';

            /* Check if we have the full request (double CRLF) */
            char *hdr_end = strstr((char *)req_buf, "\r\n\r\n");
            if (hdr_end != NULL)
            {
                int complete = 1;

                /* If it's a POST, we need the body payload */
                if (strncmp((char *)req_buf, "POST", 4) == 0)
                {
                    char *cl_str = strstr((char *)req_buf, "Content-Length:");
                    if (!cl_str) cl_str = strstr((char *)req_buf, "content-length:");
                    if (!cl_str) cl_str = strstr((char *)req_buf, "Content-length:");

                    if (cl_str && cl_str < hdr_end)
                    {
                        int content_len = atoi(cl_str + 15);
                        int header_len = (hdr_end + 4) - (char *)req_buf;
                        if (total_recv < header_len + content_len)
                        {
                            complete = 0; /* Keep reading body */
                        }
                    }
                }

                if (complete)
                {
                    break;
                }
            }
        }
    }

    if (total_recv <= 0)
    {
        LOG_DEBUG(TAG_PORT, "Empty request, closing");
        return 0;
    }

    char *request = (char *)req_buf;
    LOG_DEBUG(TAG_PORT, "HTTP request (%ld bytes): %.60s", (long)total_recv, request);

    /* ── Route: GET / → Config Page ──────────────────── */
    if (strncmp(request, "GET / ", 6) == 0 ||
        strncmp(request, "GET /index", 10) == 0)
    {
        LOG_INFO(TAG_PORT, "Serving config page");
        _send_html_page(client_sock, HTML_CONFIG_BODY);
        return 0;
    }

    /* ── Route: POST /configure → Save credentials ───── */
    if (strncmp(request, "POST /configure", 15) == 0)
    {
        LOG_INFO(TAG_PORT, "Processing WiFi configuration...");

        /* Find POST body (after headers) */
        const char *body = strstr(request, "\r\n\r\n");
        if (body == NULL)
        {
            LOG_ERROR(TAG_PORT, "POST body not found");
            _portal_send_all(client_sock,
                             (const uint8_t *)HTTP_REDIRECT_RESPONSE,
                             (int32_t)strlen(HTTP_REDIRECT_RESPONSE));
            return 0;
        }
        body += 4;  /* Skip \r\n\r\n */

        /* Extract SSID and Password from form data */
        char ssid[33] = {0};
        char password[64] = {0};

        if (_extract_form_field(body, "ssid", ssid, sizeof(ssid)) != 0 ||
            strlen(ssid) == 0)
        {
            LOG_ERROR(TAG_PORT, "Missing or empty SSID in form data");
            _portal_send_all(client_sock,
                             (const uint8_t *)HTTP_REDIRECT_RESPONSE,
                             (int32_t)strlen(HTTP_REDIRECT_RESPONSE));
            return 0;
        }

        if (_extract_form_field(body, "password", password, sizeof(password)) != 0)
        {
            LOG_WARN(TAG_PORT, "No password field — using empty (open network)");
            password[0] = '\0';
        }

        LOG_INFO(TAG_PORT, "Received credentials: SSID='%s' PWD=(%u chars)",
                 ssid, (unsigned)strlen(password));

        /* ── Deferred Validation Architecture ──
         * Save credentials to flash immediately, then reboot.
         * The boot flow in main.c will test the connection:
         *   - Success → normal operation
         *   - Failure → re-enters captive portal automatically
         *
         * Why not test-before-save? The EMW3080 cannot reliably serve
         * the SoftAP HTTP connection while simultaneously performing
         * a STA connect (radio channel switch kills the TCP socket).
         * Every IoT provisioning system (ESP-IDF, Arduino WiFiManager)
         * uses this deferred approach for the same reason. */

        /* ── Step 1: Save to flash ── */
        WiFiCredStatus_t save_ret = WiFiCred_Save(ssid, password);
        if (save_ret != WIFI_CRED_OK)
        {
            LOG_ERROR(TAG_PORT, "FATAL: Failed to save credentials to flash!");
            _send_html_page(client_sock, HTML_ERROR_BODY);
            return 0;
        }

        LOG_INFO(TAG_PORT, "Credentials saved to flash OK");

        /* ── Step 2: Send success page ── */
        _send_html_page(client_sock, HTML_SUCCESS_BODY);

        /* Give the client time to receive the full response */
        MX_WIFI_IO_YIELD(wifi_obj_get(), 500);
        return 1;  /* Signal reboot needed */

    }

    /* ── Captive Portal Detection URLs ───────────────── */
    /* Apple:   GET /hotspot-detect.html
     * Android: GET /generate_204
     * Windows: GET /connecttest.txt
     * All → redirect to our config page */
    if (strstr(request, "GET /hotspot-detect") != NULL ||
        strstr(request, "GET /generate_204") != NULL ||
        strstr(request, "GET /connecttest") != NULL ||
        strstr(request, "GET /ncsi.txt") != NULL ||
        strstr(request, "GET /redirect") != NULL ||
        strstr(request, "GET /favicon.ico") != NULL)
    {
        LOG_INFO(TAG_PORT, "Captive portal detection → serving config directly");
        /* Apple/Android captive portal probes expect a direct 200 OK + HTML body */
        _send_html_page(client_sock, HTML_CONFIG_BODY);
        return 0;
    }

    /* ── Unknown route → redirect to / ───────────────── */
    LOG_DEBUG(TAG_PORT, "Unknown route → redirect to /");
    _portal_send_all(client_sock,
                     (const uint8_t *)HTTP_REDIRECT_RESPONSE,
                     (int32_t)strlen(HTTP_REDIRECT_RESPONSE));
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SoftAP Setup
 * ═══════════════════════════════════════════════════════════════════════════ */

static PortalStatus_t _start_softap(void)
{
    MX_WIFIObject_t *wifi = wifi_obj_get();

    /* Build AP SSID using last 2 bytes of MAC for uniqueness */
    MX_WIFI_APSettings_t ap_settings;
    memset(&ap_settings, 0, sizeof(ap_settings));

    snprintf(ap_settings.SSID, sizeof(ap_settings.SSID),
             "IoT-Setup-%02X%02X",
             wifi->SysInfo.MAC[4], wifi->SysInfo.MAC[5]);

    /* WPA2 network for first-time setup (open unsupported by EMW3080) */
    strncpy(ap_settings.pswd, "setup123", sizeof(ap_settings.pswd) - 1);
    ap_settings.channel = PORTAL_AP_CHANNEL;

    /* IP configuration */
    strncpy(ap_settings.ip.localip, PORTAL_AP_IP, sizeof(ap_settings.ip.localip) - 1);
    strncpy(ap_settings.ip.netmask, PORTAL_AP_NETMASK, sizeof(ap_settings.ip.netmask) - 1);
    strncpy(ap_settings.ip.gateway, PORTAL_AP_GATEWAY, sizeof(ap_settings.ip.gateway) - 1);
    strncpy(ap_settings.ip.dnserver, PORTAL_AP_IP, sizeof(ap_settings.ip.dnserver) - 1);

    LOG_INFO(TAG_PORT, "Starting SoftAP: '%s' on channel %d",
             ap_settings.SSID, ap_settings.channel);
    LOG_INFO(TAG_PORT, "AP IP: %s", PORTAL_AP_IP);

    int32_t ret = MX_WIFI_StartAP(wifi, &ap_settings);
    if (ret != MX_WIFI_STATUS_OK)
    {
        LOG_ERROR(TAG_PORT, "MX_WIFI_StartAP failed (err=%ld)", (long)ret);
        return PORTAL_ERROR_AP;
    }

    /* Wait for AP to be ready */
    MX_WIFI_IO_YIELD(wifi, 3000);

    LOG_INFO(TAG_PORT, "SoftAP started OK: '%s'", ap_settings.SSID);
    return PORTAL_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TCP Server Setup
 * ═══════════════════════════════════════════════════════════════════════════ */

static PortalStatus_t _start_http_server(void)
{
    MX_WIFIObject_t *wifi = wifi_obj_get();

    /* Create TCP socket */
    s_http_server_sock = MX_WIFI_Socket_create(
        wifi, MX_AF_INET, MX_SOCK_STREAM, MX_IPPROTO_TCP);
    if (s_http_server_sock < 0)
    {
        LOG_ERROR(TAG_PORT, "HTTP socket create failed");
        return PORTAL_ERROR_SERVER;
    }

    /* Bind to port 80 */
    struct mx_sockaddr_in bind_addr = {0};
    bind_addr.sin_len    = (uint8_t)sizeof(bind_addr);
    bind_addr.sin_family = MX_AF_INET;
    bind_addr.sin_port   = (uint16_t)((PORTAL_HTTP_PORT >> 8) |
                            ((PORTAL_HTTP_PORT & 0xFF) << 8));
    bind_addr.sin_addr.s_addr = 0;  /* INADDR_ANY */

    int32_t ret = MX_WIFI_Socket_bind(
        wifi, s_http_server_sock,
        (struct mx_sockaddr *)&bind_addr,
        (int32_t)sizeof(bind_addr));
    if (ret < 0)
    {
        LOG_ERROR(TAG_PORT, "HTTP socket bind failed (err=%ld)", (long)ret);
        MX_WIFI_Socket_close(wifi, s_http_server_sock);
        s_http_server_sock = -1;
        return PORTAL_ERROR_SERVER;
    }

    /* Listen with small backlog */
    ret = MX_WIFI_Socket_listen(wifi, s_http_server_sock, PORTAL_MAX_CONNECTIONS);
    if (ret < 0)
    {
        LOG_ERROR(TAG_PORT, "HTTP socket listen failed (err=%ld)", (long)ret);
        MX_WIFI_Socket_close(wifi, s_http_server_sock);
        s_http_server_sock = -1;
        return PORTAL_ERROR_SERVER;
    }

    LOG_INFO(TAG_PORT, "HTTP server listening on port %d", PORTAL_HTTP_PORT);
    return PORTAL_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DNS Redirect Server Setup
 * ═══════════════════════════════════════════════════════════════════════════ */

static PortalStatus_t _start_dns_server(void)
{
    MX_WIFIObject_t *wifi = wifi_obj_get();

    /* Create UDP socket for DNS */
    s_dns_sock = MX_WIFI_Socket_create(
        wifi, MX_AF_INET, MX_SOCK_DGRAM, MX_IPPROTO_UDP);
    if (s_dns_sock < 0)
    {
        LOG_WARN(TAG_PORT, "DNS socket create failed — portal will work without auto-detect");
        return PORTAL_ERROR_DNS;
    }

    /* Bind to port 53 */
    struct mx_sockaddr_in dns_addr = {0};
    dns_addr.sin_len    = (uint8_t)sizeof(dns_addr);
    dns_addr.sin_family = MX_AF_INET;
    dns_addr.sin_port   = (uint16_t)((PORTAL_DNS_PORT >> 8) |
                           ((PORTAL_DNS_PORT & 0xFF) << 8));
    dns_addr.sin_addr.s_addr = 0;

    int32_t ret = MX_WIFI_Socket_bind(
        wifi, s_dns_sock,
        (struct mx_sockaddr *)&dns_addr,
        (int32_t)sizeof(dns_addr));
    if (ret < 0)
    {
        LOG_WARN(TAG_PORT, "DNS socket bind failed — captive detect may not work");
        MX_WIFI_Socket_close(wifi, s_dns_sock);
        s_dns_sock = -1;
        return PORTAL_ERROR_DNS;
    }

    /* Set non-blocking receive timeout */
    int32_t timeout_ms = 100;
    MX_WIFI_Socket_setsockopt(wifi, s_dns_sock, MX_SOL_SOCKET,
                               MX_SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

    LOG_INFO(TAG_PORT, "DNS redirect server listening on port %d", PORTAL_DNS_PORT);
    return PORTAL_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Portal Start — Main Entry Point (Blocking)
 * ═══════════════════════════════════════════════════════════════════════════ */

PortalStatus_t CaptivePortal_Start(PortalReason_t reason)
{
    LOG_INFO(TAG_PORT, "╔══════════════════════════════════════════╗");
    LOG_INFO(TAG_PORT, "║      CAPTIVE PORTAL MODE STARTING       ║");
    LOG_INFO(TAG_PORT, "╚══════════════════════════════════════════╝");
    LOG_INFO(TAG_PORT, "Reason: %s",
             reason == PORTAL_REASON_CREDS_SUSPECT ? "credentials suspect (AP present, repeated auth failure)" :
             reason == PORTAL_REASON_MANUAL        ? "manual (operator requested)" :
                                                       "fresh (no stored credentials)");

    s_portal_active = 1;
    s_portal_reason = reason;

    /* ── Step 0: Ensure WiFi driver is initialized ────────────── */
    /* Since we removed the compile-time fallback, a fresh board with no
     * stored flash credentials jumps straight to the portal without ever
     * powering on the Wi-Fi module in main.c's connection loop. */
    if (WiFi_Init() != WIFI_OK)
    {
        LOG_ERROR(TAG_PORT, "FATAL: WiFi module initialization failed. Cannot start portal.");
        return PORTAL_ERROR_AP;
    }

    /* Distinct LED language: PORTAL_CREDS_SUSPECT (RED solid + GREEN slow
     * pulse) makes it visually obvious "I stopped trying to connect on
     * purpose" versus plain PORTAL_ACTIVE (RED slow blink) for a fresh
     * device or a deliberate button-hold entry — see board_status.h. */
    BoardStatus_Set(reason == PORTAL_REASON_CREDS_SUSPECT
                     ? BOARD_STATUS_PORTAL_CREDS_SUSPECT
                     : BOARD_STATUS_PORTAL_ACTIVE);

    /* ── Step 1: Start SoftAP ─────────────────────────── */
    PortalStatus_t status = _start_softap();
    if (status != PORTAL_OK)
    {
        LOG_ERROR(TAG_PORT, "SoftAP start failed — cannot serve portal");
        CaptivePortal_Stop();  /* Releases the PORTAL_* BoardStatus override too — see
                                 * CaptivePortal_Stop()'s comment; safe even though nothing
                                 * actually started (socket closes/StopAP are no-ops on
                                 * already-unset state). */
        return status;
    }

    /* ── Step 2: Start DNS redirect (non-fatal if fails) ─ */
    _start_dns_server();

    /* ── Step 3: Start HTTP server ────────────────────── */
    status = _start_http_server();
    if (status != PORTAL_OK)
    {
        LOG_ERROR(TAG_PORT, "HTTP server start failed");
        CaptivePortal_Stop();
        return status;
    }

    /* ── Step 4: Accept loop — serve pages until configured ── */
    LOG_INFO(TAG_PORT, "Portal ready — waiting for client connections...");
    LOG_INFO(TAG_PORT, "Connect to WiFi and open http://%s/", PORTAL_AP_IP);

    MX_WIFIObject_t *wifi = wifi_obj_get();

    /* Set accept timeout so we can poll DNS and refresh watchdog */
    int32_t accept_timeout = PORTAL_ACCEPT_TIMEOUT_MS;
    MX_WIFI_Socket_setsockopt(wifi, s_http_server_sock, MX_SOL_SOCKET,
                               MX_SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));

    uint32_t last_sanity_recheck = HAL_GetTick();

    while (s_portal_active)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;  /* Refresh watchdog */
#endif

        /* Process DNS queries (non-blocking) */
        _dns_process_query();

        /* Yield SPI pipeline */
        MX_WIFI_IO_YIELD(wifi, 50);

        /* Render whatever BoardStatus_Set() picked above (PORTAL_ACTIVE or
         * PORTAL_CREDS_SUSPECT) — replaces the old ad-hoc RED toggle. */
        BoardStatus_Tick();

        /* ── Background sanity re-check (creds-suspect only) ──
         * The EMW3080 cannot reliably run SoftAP + a STA connect attempt
         * at the same time — a channel switch for the STA handshake kills
         * the SoftAP's own TCP sockets (see WiFiCred module notes / the
         * deferred test-before-save design elsewhere in this file). So
         * this is not a silent background thread: it briefly tears the
         * portal down, makes ONE bounded connect attempt with the OLD
         * stored credentials, and either reboots into normal operation
         * (success) or brings the portal back up (still failing) — at
         * most once every WIFI_PORTAL_SANITY_RECHECK_MS, so a flaky
         * router isn't hammered with repeated failed auth attempts. */
        if (s_portal_reason == PORTAL_REASON_CREDS_SUSPECT &&
            (HAL_GetTick() - last_sanity_recheck) >= WIFI_PORTAL_SANITY_RECHECK_MS)
        {
            last_sanity_recheck = HAL_GetTick();
            WiFiCredentials_t old_creds;

            if (WiFiCred_Load(&old_creds) == WIFI_CRED_OK)
            {
                LOG_INFO(TAG_PORT, "Sanity recheck: briefly leaving the portal to retry "
                         "stored credentials for '%s'...", old_creds.ssid);

                MX_WIFI_Socket_close(wifi, s_http_server_sock);
                s_http_server_sock = -1;
                if (s_dns_sock >= 0)
                {
                    MX_WIFI_Socket_close(wifi, s_dns_sock);
                    s_dns_sock = -1;
                }
                MX_WIFI_StopAP(wifi);
                MX_WIFI_IO_YIELD(wifi, 500);

                if (WiFi_Connect(old_creds.ssid, old_creds.password) == WIFI_OK)
                {
                    LOG_INFO(TAG_PORT, "Sanity recheck SUCCEEDED — stored credentials work "
                             "again. Rebooting into normal operation, no user action needed.");
                    HAL_Delay(300);
                    NVIC_SystemReset();
                    /* Never returns */
                }

                LOG_WARN(TAG_PORT, "Sanity recheck failed — still can't connect. "
                         "Restarting portal.");
                MX_WIFI_Disconnect(wifi);
                MX_WIFI_IO_YIELD(wifi, 300);

                if (_start_softap() != PORTAL_OK || _start_dns_server() != PORTAL_OK ||
                    _start_http_server() != PORTAL_OK)
                {
                    LOG_ERROR(TAG_PORT, "Failed to restart portal after sanity recheck — rebooting");
                    NVIC_SystemReset();
                }
                wifi = wifi_obj_get();
                MX_WIFI_Socket_setsockopt(wifi, s_http_server_sock, MX_SOL_SOCKET,
                                           MX_SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));
                BoardStatus_Set(BOARD_STATUS_PORTAL_CREDS_SUSPECT);
            }
        }

        /* Accept HTTP connection (non-blocking with timeout) */
        struct mx_sockaddr client_addr;
        uint32_t addr_len = sizeof(client_addr);

        int32_t client_sock = MX_WIFI_Socket_accept(
            wifi, s_http_server_sock,
            &client_addr, &addr_len);

        if (client_sock < 0)
        {
            continue;  /* Timeout — loop back to DNS/yield */
        }

        LOG_INFO(TAG_PORT, "HTTP client connected (sock=%ld)", (long)client_sock);
        BoardStatus_Pulse(BOARD_PULSE_PORTAL_CLIENT);

        /* Explicitly set receive timeout to 100ms. EMW3080 accept() creates new sockets
         * with the default 10000ms blocking timeout instead of inheriting from the server socket.
         * Without this, speculative browser connections cause a 10s deadlock (Command 0x0205 timeout). */
        int32_t rcv_timeout = 100;
        MX_WIFI_Socket_setsockopt(wifi, client_sock, MX_SOL_SOCKET, MX_SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        /* Handle the request */
        int configured = _handle_http_client(client_sock);

        /* Close client socket */
        MX_WIFI_Socket_close(wifi, client_sock);

        if (configured)
        {
            LOG_INFO(TAG_PORT, "Credentials saved — initiating reboot...");

            /* Clean shutdown */
            CaptivePortal_Stop();

            /* Success indication: rapid GREEN blinks */
            for (int i = 0; i < 6; i++)
            {
                BSP_LED_Toggle(LED_GREEN);
                HAL_Delay(150);
            }

            /* ── System Reboot ── */
            HAL_Delay(2000);  /* Let success page render on client */
            LOG_INFO(TAG_PORT, "=== REBOOTING ===");
            NVIC_SystemReset();
            /* Never returns */
        }
    }

    CaptivePortal_Stop();
    return PORTAL_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Portal Stop — Tear down
 * ═══════════════════════════════════════════════════════════════════════════ */

void CaptivePortal_Stop(void)
{
    MX_WIFIObject_t *wifi = wifi_obj_get();

    s_portal_active = 0;

    if (s_dns_sock >= 0)
    {
        MX_WIFI_Socket_close(wifi, s_dns_sock);
        s_dns_sock = -1;
    }

    if (s_http_server_sock >= 0)
    {
        MX_WIFI_Socket_close(wifi, s_http_server_sock);
        s_http_server_sock = -1;
    }

    MX_WIFI_StopAP(wifi);

    /* Release BOTH possible portal overrides — PORTAL_ACTIVE and
     * PORTAL_CREDS_SUSPECT are mutually exclusive so only one is ever
     * actually active, but Clear() is a no-op if `state` isn't the
     * current override, so calling both unconditionally is safe and
     * covers every reason the portal could have been started with.
     * (2026-08-20 verification round 3: previously NEITHER was ever
     * cleared anywhere in the firmware — a failed SoftAP/HTTP bring-up,
     * or any call to CaptivePortal_Stop(), left the tier-2 override
     * latched forever, permanently masking OTA/WiFi/MQTT LED states for
     * the rest of the device's uptime.) */
    BoardStatus_Clear(BOARD_STATUS_PORTAL_ACTIVE);
    BoardStatus_Clear(BOARD_STATUS_PORTAL_CREDS_SUSPECT);

    LOG_INFO(TAG_PORT, "Captive portal stopped");
}
