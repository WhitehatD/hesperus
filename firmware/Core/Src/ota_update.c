/**
 * @file    ota_update.c
 * @brief   Over-The-Air Firmware Update — Dual-Bank Flash OTA
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Enterprise OTA implementation for the STM32U585AI (2MB dual-bank flash).
 * (Note: Comment added to trigger CI firmware build job)
 *
 * Flash Layout (STM32U585AI):
 *   Bank 1: 0x0800_0000 – 0x080F_FFFF  (1 MB, 128 pages × 8 KB)
 *   Bank 2: 0x0810_0000 – 0x081F_FFFF  (1 MB, 128 pages × 8 KB)
 *
 * The active bank is selected by the SWAP_BANK option bit (OPTR register).
 * When SWAP_BANK = 0: Bank 1 is mapped at 0x0800_0000 (active).
 * When SWAP_BANK = 1: Bank 2 is mapped at 0x0800_0000 (active).
 *
 * OTA writes to the INACTIVE bank, then swaps via Option Bytes.
 */

#include "ota_update.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "wifi.h"
#include "mqtt_handler.h"
#include "main.h"

#include "mx_wifi.h"
#include "mx_wifi_io.h"
#include "mx_address.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FLASH_BANK1_BASE    0x08000000UL
#define FLASH_BANK2_BASE    0x08100000UL
#ifndef FLASH_BANK_SIZE
#define FLASH_BANK_SIZE     (1024 * 1024)    /* 1 MB per bank */
#endif
#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE     (8 * 1024)       /* 8 KB per page */
#endif
#define FLASH_PAGES_PER_BANK 128
/* Trigger CI — 2026-03-25T23:06 OTA final verification */
/* TAMP Backup Register for boot counter (BKP0R..BKP3R may be used by HAL,
 * so we use BKP4R to avoid conflicts).
 * On STM32U5, backup registers are in the TAMP peripheral, NOT RTC. */
#define OTA_BOOT_COUNTER_REG   TAMP->BKP4R
#define OTA_MAX_BOOT_FAILURES  3

/* ═══════════════════════════════════════════════════════════════════════════
 *  CRC32 (Software — MPEG-2 variant, no lookup table)
 *
 *  We use software CRC32 instead of the STM32 hardware CRC peripheral
 *  because the HW CRC uses a non-standard polynomial and would require
 *  the server to match it. Standard CRC32 is universally supported.
 *
 *  Exposed via ota_update.h (was file-local) so wifi.c's chunked image
 *  upload can compute a payload CRC using the SAME algorithm the server's
 *  _compute_crc32() (server/app/api/firmware_routes.py) already matches for
 *  OTA — one CRC convention for the whole codebase instead of two
 *  implementations that could silently drift apart.
 * ═══════════════════════════════════════════════════════════════════════════ */

uint32_t Firmware_CRC32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= ((uint32_t)data[i] << 24);
        for (int bit = 0; bit < 8; bit++)
        {
            if (crc & 0x80000000UL)
                crc = (crc << 1) ^ 0x04C11DB7UL;
            else
                crc = (crc << 1);
        }
    }
    return crc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEC-06: Semantic Version Comparator (anti-downgrade)
 *
 *  Parses "major.minor" strings and returns 1 if 'candidate'
 *  is strictly newer than 'current'. Rejects equal or older versions.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int _is_version_newer(const char *candidate, const char *current)
{
    /* Format: major.minor.patch.build
     * - major/minor/patch: semantic version components
     * - build: Unix epoch injected at compile-time (CI passes it via make BUILD_EPOCH=...)
     *          so every CI build has a unique version even when patch is unchanged.
     * Boards without a build component default to 0 (old firmware — always updatable).
     */
    int c_major = 0, c_minor = 0, c_patch = 0, c_build = 0;
    int n_major = 0, n_minor = 0, n_patch = 0, n_build = 0;

    sscanf(current,   "%d.%d.%d.%d", &c_major, &c_minor, &c_patch, &c_build);
    sscanf(candidate, "%d.%d.%d.%d", &n_major, &n_minor, &n_patch, &n_build);

    if (strchr(current, '.') == NULL || strchr(candidate, '.') == NULL)
    {
        /* Non-semantic fallback: reject equal strings to prevent infinite loop */
        return (strcmp(candidate, current) != 0) ? 1 : 0;
    }

    if (n_major > c_major) return 1;
    if (n_major == c_major && n_minor > c_minor) return 1;
    if (n_major == c_major && n_minor == c_minor && n_patch > c_patch) return 1;
    if (n_major == c_major && n_minor == c_minor && n_patch == c_patch && n_build > c_build) return 1;
    return 0;  /* Equal or older */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Flash Bank Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Determine which bank is currently active.
 * @retval FLASH_BANK_1 or FLASH_BANK_2
 */
static uint32_t _get_active_bank(void)
{
    /* Read the SWAP_BANK bit from the current option bytes */
    FLASH_OBProgramInitTypeDef ob_cfg;
    HAL_FLASHEx_OBGetConfig(&ob_cfg);

    if (ob_cfg.USERConfig & FLASH_OPTR_SWAP_BANK)
        return FLASH_BANK_2;
    else
        return FLASH_BANK_1;
}

/**
 * @brief  Get the inactive bank (the one we write OTA updates to).
 * @retval FLASH_BANK_1 or FLASH_BANK_2
 */
static uint32_t _get_inactive_bank(void)
{
    return (_get_active_bank() == FLASH_BANK_1) ? FLASH_BANK_2 : FLASH_BANK_1;
}

/**
 * @brief  Get the base address of the inactive bank.
 *
 * The SWAP_BANK option bit controls which physical bank is mapped at
 * 0x0800_0000 (active/executing) and which at 0x0810_0000 (inactive).
 * Regardless of which physical bank is "inactive", it is ALWAYS mapped
 * at 0x0810_0000 in the CPU address space — that's the whole point of
 * the swap mechanism.
 */
static uint32_t _get_inactive_bank_base(void)
{
    return FLASH_BANK2_BASE;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TCP Socket Helpers (reused from wifi.c pattern)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Enterprise-grade Watchdog-Safe Delay Wrapper */
static void _ota_yield_with_watchdog(uint32_t delay_ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < delay_ms)
    {
        uint32_t chunk = (delay_ms - (HAL_GetTick() - start));
        if (chunk > 100) chunk = 100;
        MX_WIFI_IO_YIELD(wifi_obj_get(), chunk);
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu; /* Pet the dog frequently during yield */
#endif
    }
}

static int32_t _ota_socket_open(void)
{
    int32_t sock = WiFi_TcpConnect(SERVER_HOST, SERVER_PORT);
    if (sock >= 0)
    {
        /* ENTERPRISE FIX: Override receive timeout to 1000ms.
         * The EMW3080 AT firmware ignores sub-500ms SO_RCVTIMEO values
         * and falls back to MX_WIFI_CMD_TIMEOUT (10s). 1000ms is reliably
         * honored, preventing recv() from blocking the entire MIPC layer. 
         * NOTE: MX_WIFI_Socket_setsockopt explicitly expects a 4-byte int32_t
         * representing milliseconds. Passing an 8-byte POSIX timeval struct
         * corrupts the AT firmware's parser, causing infinite block deadlocks. */
        int32_t rcv_timeout = 1000; /* 1s timeout */
        MX_WIFI_Socket_setsockopt(wifi_obj_get(), sock, MX_SOL_SOCKET, MX_SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    }
    return sock;
}

static int _ota_send_all(int32_t sock, const uint8_t *data, int32_t len)
{
    int32_t offset = 0;
    int retries = 0;

    while (offset < len)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;
#endif
        int32_t chunk = len - offset;
        if (chunk > OTA_DOWNLOAD_CHUNK_SIZE)
            chunk = OTA_DOWNLOAD_CHUNK_SIZE;

        /* flags MUST be 0 — mx_wifi.h documents "flags: zero for MXOS".
         * HTTP_RESPONSE_TIMEOUT_MS was being forwarded as wire-protocol
         * flags to the module (mx_wifi.c: cp->flags = flags), not used as
         * a timeout — the real per-command wait is the fixed
         * MX_WIFI_CMD_TIMEOUT in mx_wifi_conf.h, unaffected by this value.
         * Low practical impact here (OTA request bodies are small enough
         * to rarely hit backpressure), but it's the same vendor-contract
         * violation as the one that broke image upload — fixed for
         * correctness, see wifi.c:_socket_send_all for the full story. */
        int32_t sent = MX_WIFI_Socket_send(
            wifi_obj_get(), sock,
            (uint8_t *)(data + offset), chunk,
            0);

        if (sent > 0)
        {
            offset += sent;
            retries = 0;
            if (offset < len)
                _ota_yield_with_watchdog(10);
        }
        else
        {
            retries++;
            if (retries >= 3) return -1;
            _ota_yield_with_watchdog(100);
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Bounded-Retry Socket Receive
 *
 *  2026-08-21: previously called a hand-patched vendor-driver function
 *  (MX_WIFI_Socket_recv_timeout, since removed from mx_wifi.c/.h) that
 *  duplicated the stock recv() but with a caller-supplied MIPC transport
 *  timeout. That's two different timeouts conflated: the socket-level
 *  SO_RCVTIMEO (how long the MODULE waits for TCP data before answering
 *  the recv command — set once in _ota_socket_open, 1000ms, confirmed by
 *  hardware testing to be the smallest value the AT firmware reliably
 *  honors, see that function) vs. the LOCAL wait for the module's SPI
 *  reply to arrive at all (stock recv() uses the fixed MX_WIFI_CMD_TIMEOUT
 *  = 10000ms — a safety ceiling, not something normally hit, since the
 *  module should answer within its SO_RCVTIMEO window regardless). Now
 *  uses stock MX_WIFI_Socket_recv() and lets the already-correct
 *  SO_RCVTIMEO govern module-side wait; this outer loop still re-polls up
 *  to total_timeout_ms, same as before.
 *
 *  Chunk size aligned to 1460 bytes (TCP MSS) to prevent MIPC
 *  fragmentation and maximize per-recv throughput.
 * ═══════════════════════════════════════════════════════════════════════════ */
static int32_t _ota_safe_recv(int32_t sock, uint8_t *buf, int32_t max_len, uint32_t total_timeout_ms)
{
    uint32_t start = HAL_GetTick();
    int32_t ret = 0;
    uint32_t polls = 0;

    while ((HAL_GetTick() - start) < total_timeout_ms)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;
#endif
        /* MIPC/SPI Payload Allocation Fault mitigation.
         * Although standard Ethernet MSS is 1460, EMW3080 AT firmware internally
         * allocates MIPC structures from a very small pool. If we request 1460
         * bytes when >1460 bytes are buffered, it fails to allocate an IPC packet
         * large enough to encapsulate it, immediately dropping the command and
         * starving the STM32 for `total_timeout_ms` (15s). 1000 bytes is safe. */
        int32_t safe_chunk = (max_len > 1000) ? 1000 : max_len;

        ret = MX_WIFI_Socket_recv(wifi_obj_get(), sock, buf, safe_chunk, 0);
        polls++;

        if (ret > 0)
        {
            LOG_DEBUG(TAG_OTA, "recv: %ld bytes after %lu polls (%lums)",
                      (long)ret, (unsigned long)polls,
                      (unsigned long)(HAL_GetTick() - start));
            return ret;
        }

        /* Log progress every 5 polls so serial output shows liveness */
        if (polls % 5 == 0)
        {
            LOG_DEBUG(TAG_OTA, "recv polling: %lu polls, %lums elapsed, last ret=%ld",
                      (unsigned long)polls,
                      (unsigned long)(HAL_GetTick() - start),
                      (long)ret);
        }
    }

    LOG_WARN(TAG_OTA, "recv timeout: %lu polls in %lums, last ret=%ld",
             (unsigned long)polls,
             (unsigned long)(HAL_GetTick() - start),
             (long)ret);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Boot Validation (RTC Backup Register Counter)
 * ═══════════════════════════════════════════════════════════════════════════ */

void OTA_ValidateBoot(void)
{
    /* Enable backup domain access */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    uint32_t boot_count = OTA_BOOT_COUNTER_REG;

    LOG_INFO(TAG_OTA, "Boot counter: %lu / %d",
             (unsigned long)boot_count, OTA_MAX_BOOT_FAILURES);

    if (boot_count >= OTA_MAX_BOOT_FAILURES)
    {
        LOG_ERROR(TAG_OTA, "CRITICAL: %lu consecutive boot failures — REVERTING FIRMWARE",
                  (unsigned long)boot_count);

        /* Reset counter before swap to prevent infinite rollback loop */
        OTA_BOOT_COUNTER_REG = 0;

        /* Swap back to previous bank */
        OTA_SwapBankAndReset();

        /* If swap failed, continue with current firmware */
        LOG_ERROR(TAG_OTA, "Bank swap failed — continuing with current firmware");
    }

    /* Increment boot counter — will be cleared by OTA_MarkBootSuccessful() */
    OTA_BOOT_COUNTER_REG = boot_count + 1;
}

void OTA_MarkBootSuccessful(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    OTA_BOOT_COUNTER_REG = 0;

    LOG_INFO(TAG_OTA, "Boot validated — counter reset to 0");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Version Check (HTTP GET /api/firmware/version)
 * ═══════════════════════════════════════════════════════════════════════════ */

OTAStatus_t OTA_CheckForUpdate(OTAVersionInfo_t *info)
{
    if (info == NULL) return OTA_ERROR_PARSE;

    LOG_INFO(TAG_OTA, "Checking for firmware update (current: %s)...", FW_VERSION);

    /* Build HTTP GET request */
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Firmware-Token: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        OTA_VERSION_PATH, SERVER_HOST, SERVER_PORT, FIRMWARE_UPLOAD_TOKEN);

    /* Open socket */
    int32_t sock = _ota_socket_open();
    if (sock < 0)
    {
        LOG_ERROR(TAG_OTA, "Version check: socket connect failed");
        return OTA_ERROR_NETWORK;
    }

    /* Send request */
    if (_ota_send_all(sock, (uint8_t *)header, header_len) != 0)
    {
        LOG_ERROR(TAG_OTA, "Version check: send failed");
        MX_WIFI_Socket_close(wifi_obj_get(), sock);
        return OTA_ERROR_NETWORK;
    }

    /* Receive response. 2026-08-24: was 8000ms — SMALLER than
     * MX_WIFI_CMD_TIMEOUT (10000ms, the module's own ceiling for a single
     * MX_WIFI_Socket_recv() poll — see _ota_safe_recv's header comment).
     * _ota_safe_recv's outer while-loop only checks elapsed time BETWEEN
     * polls, never during one — it cannot preempt a poll already in
     * flight. So whenever the module actually hit its own ceiling on the
     * FIRST poll, the call was structurally guaranteed to overshoot the
     * requested budget and return after one poll regardless of what
     * total_timeout_ms said: exactly the reproduced symptom ("recv
     * timeout: 1 polls in 10078ms, last ret=-1" against an 8000ms ask).
     * 12000ms gives a genuine 10s-ceiling poll room to finish, and a
     * second, cheap poll after it if that one comes back fast. Same
     * ceiling now used for the download-progress recv calls below
     * (15000ms) for consistency, which were already comfortably above it.
     * NOT hardware-validated tonight (board has no USB/ST-Link attached) —
     * confirm the recv-timeout log line stops recurring next session. */
    uint8_t resp[512] = {0};
    int32_t resp_len = _ota_safe_recv(sock, resp, sizeof(resp) - 1, 12000);

    MX_WIFI_Socket_close(wifi_obj_get(), sock);

    if (resp_len <= 0)
    {
        LOG_ERROR(TAG_OTA, "Version check: no response");
        return OTA_ERROR_NETWORK;
    }

    resp[resp_len] = '\0';

    /* Find JSON body */
    const char *hdr_end = strstr((char *)resp, "\r\n\r\n");
    if (hdr_end == NULL)
    {
        LOG_ERROR(TAG_OTA, "Version check: no HTTP body");
        return OTA_ERROR_PARSE;
    }

    const char *body = strchr(hdr_end + 4, '{');
    if (body == NULL)
    {
        LOG_ERROR(TAG_OTA, "Version check: no JSON in body");
        return OTA_ERROR_PARSE;
    }

    /* ── 5. Setup JSON Allocator and Parse ── */
    json_mem_reset();
    cJSON *root = cJSON_Parse(body);
    if (root == NULL)
    {
        LOG_ERROR(TAG_OTA, "Version check: JSON parse failed");
        return OTA_ERROR_PARSE;
    }

    cJSON *j_version = cJSON_GetObjectItem(root, "version");
    cJSON *j_size    = cJSON_GetObjectItem(root, "size");
    cJSON *j_crc32   = cJSON_GetObjectItem(root, "crc32");

    if (!cJSON_IsString(j_version) || !cJSON_IsNumber(j_size))
    {
        LOG_ERROR(TAG_OTA, "Version check: missing fields");
        cJSON_Delete(root);
        return OTA_ERROR_PARSE;
    }

    /* Copy version info */
    strncpy(info->version, j_version->valuestring, sizeof(info->version) - 1);
    info->version[sizeof(info->version) - 1] = '\0';
    info->size  = (uint32_t)j_size->valuedouble;
    info->crc32 = cJSON_IsNumber(j_crc32) ? (uint32_t)j_crc32->valuedouble : 0;

    cJSON_Delete(root);

    /* Compare versions — SEC-06: reject downgrades */
    if (!_is_version_newer(info->version, FW_VERSION))
    {
        LOG_INFO(TAG_OTA, "Firmware is up-to-date or candidate is older (current v%s, candidate v%s)", FW_VERSION, info->version);
        return OTA_NO_UPDATE;
    }

    /* Size sanity check */
    if (info->size > OTA_MAX_FW_SIZE || info->size == 0)
    {
        LOG_ERROR(TAG_OTA, "Firmware size invalid: %lu bytes (max=%lu)",
                  (unsigned long)info->size, (unsigned long)OTA_MAX_FW_SIZE);
        return OTA_ERROR_TOO_LARGE;
    }

    LOG_INFO(TAG_OTA, "Update available: v%s → v%s (%lu bytes, crc=0x%08lX)",
             FW_VERSION, info->version,
             (unsigned long)info->size, (unsigned long)info->crc32);

    return OTA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Download & Flash (HTTP GET → inactive bank write)
 * ═══════════════════════════════════════════════════════════════════════════ */

OTAStatus_t OTA_DownloadAndFlash(const OTAVersionInfo_t *info,
                                 uint8_t *ram_buffer, uint32_t ram_size)
{
    if (info == NULL || info->size == 0 || ram_buffer == NULL)
        return OTA_ERROR_PARSE;

    if (info->size > ram_size)
    {
        LOG_ERROR(TAG_OTA, "Firmware (%lu bytes) exceeds RAM buffer (%lu bytes)",
                  (unsigned long)info->size, (unsigned long)ram_size);
        return OTA_ERROR_TOO_LARGE;
    }

    uint32_t inactive_bank = _get_inactive_bank();
    uint32_t bank_base     = _get_inactive_bank_base();
    uint32_t pages_needed  = (info->size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    LOG_INFO(TAG_OTA, "Flashing v%s to Bank %lu (%lu pages to erase)...",
             info->version,
             (unsigned long)(inactive_bank == FLASH_BANK_1 ? 1 : 2),
             (unsigned long)pages_needed);

    /* ═════════════════════════════════════════════════════════════════════
     * Stage 1: Download entire firmware to RAM (NO flash ops during I/O)
     *
     * Enterprise download pipeline:
     *   1. HTTP status code validation (reject 4xx/5xx)
     *   2. Content-Length cross-check against version metadata
     *   3. MQTT progress streaming for real-time dashboard telemetry
     *   4. Download throughput metrics ([PERF] logging)
     *   5. Retry loop with exponential backoff for SPI recovery
     * ═════════════════════════════════════════════════════════════════════ */

    LOG_INFO(TAG_OTA, "Downloading firmware to RAM (%lu bytes)...",
             (unsigned long)info->size);

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Firmware-Token: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        OTA_DOWNLOAD_PATH, SERVER_HOST, SERVER_PORT, FIRMWARE_UPLOAD_TOKEN);

    /* SPI warm-up: let the EMW3080 fully drain any residual MIPC state
     * from the MQTT disconnect + Camera deinit before we start fresh
     * HTTP traffic. Without this, stale SPI frames can corrupt the
     * first socket open. */
    LOG_DEBUG(TAG_OTA, "SPI warm-up yield (2s)...");
    _ota_yield_with_watchdog(2000);

    uint32_t total_downloaded = 0;
    int download_ok = 0;
    uint32_t retry_delay_ms = OTA_DOWNLOAD_RETRY_BASE_MS;
    uint32_t download_start_tick = 0;

    for (int dl_attempt = 0; dl_attempt < OTA_DOWNLOAD_MAX_RETRIES; dl_attempt++)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu; /* Kick watchdog on new attempt */
#endif
        if (dl_attempt > 0)
        {
            LOG_WARN(TAG_OTA, "Download retry %d/%d after %lums cooldown...",
                     dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES,
                     (unsigned long)retry_delay_ms);

            /* Yield SPI aggressively so EMW3080 drains any stale MIPC state */
            _ota_yield_with_watchdog(retry_delay_ms);
            retry_delay_ms *= 2;
        }

        /* Brief SPI yield before socket open — let any pending MQTT
         * publish/keepalive traffic drain off the bus first. */
        _ota_yield_with_watchdog(500);

        total_downloaded = 0;
        download_start_tick = HAL_GetTick();

        LOG_DEBUG(TAG_OTA, "Opening TCP socket to %s:%d...", SERVER_HOST, SERVER_PORT);
        
        /* ENTERPRISE FIX: Rotate dummy UDP sockets to prevent EMW3080 socket recycling.
         * Because MQTT disconnects right before this, fd=0 is freed. 
         * If the Autonomous Daemon triggers the OTA, TWO version checks are performed
         * back-to-back, leaving both fd=1 and fd=2 in TIME_WAIT.
         * Base offset must be 3 to unconditionally burn fd=0, fd=1, and fd=2. */
        int32_t dummies[5] = {-1, -1, -1, -1, -1};
        int num_dummies = (dl_attempt % 3) + 3; 
        for (int i = 0; i < num_dummies; i++) {
            dummies[i] = MX_WIFI_Socket_create(wifi_obj_get(), MX_AF_INET, MX_SOCK_DGRAM, MX_IPPROTO_UDP);
        }
        
        int32_t sock = _ota_socket_open();
        
        if (sock < 0)
        {
            LOG_ERROR(TAG_OTA, "Download: socket connect failed (attempt %d/%d)",
                      dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
            for (int i = 0; i < 5; i++) {
                if (dummies[i] >= 0) MX_WIFI_Socket_close(wifi_obj_get(), dummies[i]);
            }
            continue;
        }
        LOG_DEBUG(TAG_OTA, "Socket opened (fd=%ld) — sending HTTP GET...", (long)sock);

        if (_ota_send_all(sock, (uint8_t *)header, header_len) != 0)
        {
            LOG_ERROR(TAG_OTA, "Download: send request failed (attempt %d/%d)",
                      dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
            MX_WIFI_Socket_close(wifi_obj_get(), sock);
            for (int i = 0; i < 5; i++) {
                if (dummies[i] >= 0) MX_WIFI_Socket_close(wifi_obj_get(), dummies[i]);
            }
            continue;
        }
        LOG_DEBUG(TAG_OTA, "HTTP GET sent (%d bytes) — waiting for response...", header_len);

        /* ENTERPRISE FIX: We must NOT yield here. The remote VPS instantly transmits 
         * up to 128KB of firmware binary at line rate. The EMW3080 AT firmware possesses
         * an extremely small internal LwIP packet pool (typically 2-4KB max per socket).
         * If the STM32 delegates 1500ms to an artificial yield, the AT firmware instantly
         * hits TCP Window Size = 0 and chokes, fatally locking up the SPI IPC command
         * parser and dropping the socket context before the first recv() is executed! */

        /* Dedicated stack buffer for HTTP header reception.
         * MUST NOT overlap ram_buffer — the old placement at
         * ram_buffer + (ram_size - 1536) corrupted firmware tail bytes. */
        uint8_t recv_buf[1536];
        uint32_t recv_buf_size = sizeof(recv_buf);
        
        /* Yield-first safe recv with 15s total timeout.
         * 15s allows 10+ poll cycles (200ms yield + 1s SO_RCVTIMEO each)
         * instead of the old 8s which only allowed 1 blocked recv. */
        int32_t recv_len = _ota_safe_recv(sock, recv_buf, recv_buf_size - 1, 15000);

        if (recv_len <= 0)
        {
            LOG_ERROR(TAG_OTA, "Download: no response (attempt %d/%d)",
                      dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
            MX_WIFI_Socket_close(wifi_obj_get(), sock);
            for (int i = 0; i < 5; i++) {
                if (dummies[i] >= 0) MX_WIFI_Socket_close(wifi_obj_get(), dummies[i]);
            }
            continue;
        }

        /* ── HTTP Status Code Validation ────────────────────────────── */
        if (recv_len >= (int32_t)recv_buf_size) recv_len = recv_buf_size - 1;
        recv_buf[recv_len] = '\0';  /* Safe — recv_buf has +512 headroom */

        int http_status = 0;
        {
            const char *space = strchr((char *)recv_buf, ' ');
            if (space != NULL)
            {
                char *endptr = NULL;
                long parsed = strtol(space + 1, &endptr, 10);
                if (endptr != space + 1 && parsed >= 100 && parsed <= 599)
                    http_status = (int)parsed;
            }
        }

        if (http_status < 200 || http_status >= 300)
        {
            LOG_ERROR(TAG_OTA, "Download: HTTP %d (expected 200) — attempt %d/%d",
                      http_status, dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
            MX_WIFI_Socket_close(wifi_obj_get(), sock);
            if (http_status == 404)
            {
                LOG_ERROR(TAG_OTA, "Firmware binary not found on server");
                return OTA_ERROR_NETWORK;  /* Don't retry 404 */
            }
            continue;
        }

        LOG_INFO(TAG_OTA, "HTTP %d OK — streaming firmware body", http_status);
        LOG_DEBUG(TAG_OTA, "Header recv: %ld bytes in first chunk", (long)recv_len);

        /* ── Content-Length Cross-Validation ────────────────────────── */
        const char *cl_hdr = strstr((char *)recv_buf, "Content-Length: ");
        if (cl_hdr == NULL)
            cl_hdr = strstr((char *)recv_buf, "content-length: ");

        if (cl_hdr != NULL)
        {
            char *endptr = NULL;
            long parsed = strtol(cl_hdr + 16, &endptr, 10);
            uint32_t content_length = (endptr != cl_hdr + 16 && parsed >= 0)
                                      ? (uint32_t)parsed : 0;
            if (content_length != info->size)
            {
                LOG_WARN(TAG_OTA, "Content-Length mismatch: HTTP=%lu expected=%lu",
                         (unsigned long)content_length, (unsigned long)info->size);
            }
            else
            {
                LOG_DEBUG(TAG_OTA, "Content-Length verified: %lu bytes",
                          (unsigned long)content_length);
            }
        }

        /* ── Find body start (\r\n\r\n) ──────────────────────────── */
        uint8_t *body_start = NULL;
        int32_t body_offset = 0;
        for (int32_t i = 0; i < recv_len - 3; i++)
        {
            if (recv_buf[i] == '\r' && recv_buf[i+1] == '\n' &&
                recv_buf[i+2] == '\r' && recv_buf[i+3] == '\n')
            {
                body_start = &recv_buf[i + 4];
                body_offset = i + 4;
                break;
            }
        }

        if (body_start == NULL)
        {
            LOG_ERROR(TAG_OTA, "Download: header boundary not found (attempt %d/%d)",
                      dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
            MX_WIFI_Socket_close(wifi_obj_get(), sock);
            continue;
        }

        /* Copy first chunk's body into RAM buffer */
        int32_t first_body_len = recv_len - body_offset;
        if (first_body_len > 0)
        {
            memcpy(ram_buffer, body_start, first_body_len);
            total_downloaded = first_body_len;
        }

        /* ── Streaming download loop with MQTT progress ──────────── */
        uint8_t toggle_led = 0;
        int stall = 0;
        uint32_t last_progress_bytes = 0;  /* Track MQTT progress interval */
        uint32_t chunk_count = 0;

        LOG_DEBUG(TAG_OTA, "Starting body download (first_body=%ld bytes already in RAM)...",
                  (long)total_downloaded);

        while (total_downloaded < info->size)
        {
#if WATCHDOG_ENABLED
            IWDG->KR = 0x0000AAAAu; /* Critical kick during large download */
#endif
            /* Progress LED blink */
            BSP_LED_On(LED_RED);
            if (toggle_led) BSP_LED_On(LED_GREEN);
            else BSP_LED_Off(LED_GREEN);
            toggle_led = !toggle_led;

            uint32_t remaining = info->size - total_downloaded;
            uint32_t chunk_max = (remaining < OTA_DOWNLOAD_CHUNK_SIZE)
                                 ? remaining : OTA_DOWNLOAD_CHUNK_SIZE;

            recv_len = _ota_safe_recv(sock, ram_buffer + total_downloaded, chunk_max, 15000);
            chunk_count++;

            if (recv_len <= 0)
            {
                LOG_ERROR(TAG_OTA, "Download stalled at %lu/%lu bytes (attempt %d/%d)",
                              (unsigned long)total_downloaded,
                              (unsigned long)info->size,
                              dl_attempt + 1, OTA_DOWNLOAD_MAX_RETRIES);
                stall = 1;
                break;
            }

            total_downloaded += recv_len;

            /* ── MQTT + serial progress every OTA_PROGRESS_INTERVAL_BYTES ── */
            if (total_downloaded - last_progress_bytes >= OTA_PROGRESS_INTERVAL_BYTES
                || total_downloaded >= info->size)
            {
                last_progress_bytes = total_downloaded;
                uint32_t pct = total_downloaded * 100 / info->size;
                uint32_t elapsed_ms = HAL_GetTick() - download_start_tick;
                uint32_t kbps_x10 = (elapsed_ms > 0)
                                    ? (total_downloaded * 10u / elapsed_ms) : 0;

                LOG_INFO(TAG_OTA, "Download: %lu/%lu bytes (%lu%%) [%lu.%lu KB/s]",
                         (unsigned long)total_downloaded,
                         (unsigned long)info->size,
                         (unsigned long)pct,
                         (unsigned long)(kbps_x10 / 10u),
                         (unsigned long)(kbps_x10 % 10u));

                /* Publish progress to dashboard via MQTT */
                char progress_msg[192];
                snprintf(progress_msg, sizeof(progress_msg),
                    "{\"status\":\"ota_progress\","
                    "\"downloaded\":%lu,\"total\":%lu,"
                    "\"percent\":%lu,\"throughput_kbps\":%lu.%lu,"
                    "\"elapsed_ms\":%lu,\"attempt\":%d}",
                    (unsigned long)total_downloaded,
                    (unsigned long)info->size,
                    (unsigned long)pct,
                    (unsigned long)(kbps_x10 / 10u),
                    (unsigned long)(kbps_x10 % 10u),
                    (unsigned long)elapsed_ms,
                    dl_attempt + 1);
                if (MQTT_IsConnected())
                    MQTT_PublishStatus(progress_msg);
            }
        }

        MX_WIFI_Socket_close(wifi_obj_get(), sock);
        for (int i = 0; i < 5; i++) {
            if (dummies[i] >= 0) MX_WIFI_Socket_close(wifi_obj_get(), dummies[i]);
        }

        if (!stall && total_downloaded >= info->size)
        {
            download_ok = 1;
            break;
        }

        /* Wait before retrying (exponential backoff) */
        LOG_WARN(TAG_OTA, "Download attempt %d failed, retrying in %lums...",
                 dl_attempt + 1, (unsigned long)retry_delay_ms);
        _ota_yield_with_watchdog(retry_delay_ms);
        retry_delay_ms = (retry_delay_ms * 2 > OTA_DOWNLOAD_RETRY_MAX_MS)
                         ? OTA_DOWNLOAD_RETRY_MAX_MS : (retry_delay_ms * 2);
    }

    BSP_LED_Off(LED_RED);
    BSP_LED_Off(LED_GREEN);

    if (!download_ok)
    {
        LOG_ERROR(TAG_OTA, "Download failed after %d attempts", OTA_DOWNLOAD_MAX_RETRIES);
        return OTA_ERROR_NETWORK;
    }

    /* ── Download Throughput Telemetry ──────────────────────────────── */
    uint32_t download_ms = HAL_GetTick() - download_start_tick;
    /* Tenths of KB/s. Integer bytes/ms floors every sub-1.0 KB/s rate to a
     * meaningless "0 KB/s" — and sub-1.0 is exactly the range a degraded link
     * produces, i.e. the case the number exists to show. */
    uint32_t throughput_x10 = (download_ms > 0)
                              ? (total_downloaded * 10u / download_ms) : 0;
    LOG_INFO(TAG_OTA, "[PERF] Download: %lu bytes in %lums (%lu.%lu KB/s)",
             (unsigned long)total_downloaded,
             (unsigned long)download_ms,
             (unsigned long)(throughput_x10 / 10u),
             (unsigned long)(throughput_x10 % 10u));
    LOG_INFO(TAG_OTA, "Download complete: %lu bytes in RAM",
             (unsigned long)total_downloaded);

    /* ── CRC32 verification on RAM buffer (before touching flash) ── */
    uint32_t crc = Firmware_CRC32(0xFFFFFFFF, ram_buffer, total_downloaded);

    if (info->crc32 != 0)
    {
        if (crc != info->crc32)
        {
            LOG_ERROR(TAG_OTA, "CRC32 mismatch: computed=0x%08lX expected=0x%08lX",
                      (unsigned long)crc, (unsigned long)info->crc32);
            return OTA_ERROR_VERIFY;
        }
        LOG_INFO(TAG_OTA, "CRC32 verified OK (0x%08lX)", (unsigned long)crc);
    }

    /* ═════════════════════════════════════════════════════════════════════
     * Stage 2: Erase flash + write from RAM (no network I/O needed)
     * ═════════════════════════════════════════════════════════════════════ */

    BSP_LED_On(LED_RED);
    BSP_LED_On(LED_GREEN);

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_cfg = {0};
    erase_cfg.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_cfg.Banks     = inactive_bank;
    erase_cfg.Page      = 0;
    erase_cfg.NbPages   = pages_needed;

    uint32_t erase_error = 0;
    HAL_StatusTypeDef hal_ret = HAL_FLASHEx_Erase(&erase_cfg, &erase_error);

    if (hal_ret != HAL_OK)
    {
        LOG_ERROR(TAG_OTA, "Flash erase failed (HAL=%d, page_err=%lu)",
                  hal_ret, (unsigned long)erase_error);
        HAL_FLASH_Lock();
        BSP_LED_Off(LED_RED);
        BSP_LED_Off(LED_GREEN);
        return OTA_ERROR_FLASH_ERASE;
    }

    LOG_INFO(TAG_OTA, "Erased %lu pages — writing %lu bytes from RAM to flash...",
             (unsigned long)pages_needed, (unsigned long)total_downloaded);

    /* Write from RAM to flash — quadword aligned (16 bytes) */
    uint32_t flash_addr = bank_base;
    uint32_t written = 0;

    /* Write aligned portion */
    uint32_t aligned_len = total_downloaded & ~0x0FUL;
    for (uint32_t i = 0; i < aligned_len; i += 16)
    {
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu; /* Kick watchdog during flash writing */
#endif
        hal_ret = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_QUADWORD,
            flash_addr,
            (uint32_t)(uintptr_t)(ram_buffer + i));

        if (hal_ret != HAL_OK)
        {
            LOG_ERROR(TAG_OTA, "Flash write failed at 0x%08lX",
                      (unsigned long)flash_addr);
            HAL_FLASH_Lock();
            BSP_LED_Off(LED_RED);
            BSP_LED_Off(LED_GREEN);
            return OTA_ERROR_FLASH_WRITE;
        }
        flash_addr += 16;
        written += 16;
    }

    /* Write final partial quadword (pad with 0xFF) */
    uint32_t remainder = total_downloaded - aligned_len;
    if (remainder > 0)
    {
        uint8_t pad_buf[16];
        memset(pad_buf, 0xFF, 16);
        memcpy(pad_buf, ram_buffer + aligned_len, remainder);

        hal_ret = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_QUADWORD,
            flash_addr,
            (uint32_t)(uintptr_t)pad_buf);

        if (hal_ret != HAL_OK)
        {
            LOG_ERROR(TAG_OTA, "Flash write (final pad) failed");
            HAL_FLASH_Lock();
            BSP_LED_Off(LED_RED);
            BSP_LED_Off(LED_GREEN);
            return OTA_ERROR_FLASH_WRITE;
        }
        written += remainder;
    }

    HAL_FLASH_Lock();
    BSP_LED_Off(LED_RED);
    BSP_LED_Off(LED_GREEN);

    LOG_INFO(TAG_OTA, "Flash write complete: %lu bytes written",
             (unsigned long)written);

    return OTA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Bank Swap & Reset
 * ═══════════════════════════════════════════════════════════════════════════ */

OTAStatus_t OTA_SwapBankAndReset(void)
{
    uint32_t active = _get_active_bank();

    LOG_INFO(TAG_OTA, "Swapping flash banks (active: Bank %lu → Bank %lu)...",
             (unsigned long)(active == FLASH_BANK_1 ? 1 : 2),
             (unsigned long)(active == FLASH_BANK_1 ? 2 : 1));

    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();

    /* Read current option bytes */
    FLASH_OBProgramInitTypeDef ob_cfg;
    HAL_FLASHEx_OBGetConfig(&ob_cfg);

    /* Toggle SWAP_BANK bit */
    ob_cfg.OptionType = OPTIONBYTE_USER;
    ob_cfg.USERType   = OB_USER_SWAP_BANK;

    if (active == FLASH_BANK_1)
        ob_cfg.USERConfig = OB_SWAP_BANK_ENABLE;   /* Swap to Bank 2 */
    else
        ob_cfg.USERConfig = OB_SWAP_BANK_DISABLE;  /* Swap to Bank 1 */

    HAL_StatusTypeDef ret = HAL_FLASHEx_OBProgram(&ob_cfg);
    if (ret != HAL_OK)
    {
        LOG_ERROR(TAG_OTA, "Option byte program failed (HAL=%d)", ret);
        HAL_FLASH_OB_Lock();
        HAL_FLASH_Lock();
        return OTA_ERROR_SWAP;
    }

    LOG_INFO(TAG_OTA, "Bank swap programmed — launching reset...");
    
    /* Success: solid GREEN for 1.5 seconds before rebooting */
    BSP_LED_Off(LED_RED);
    BSP_LED_On(LED_GREEN);
    HAL_Delay(1500);

    /* This triggers a system reset with the new bank mapping.
     * DOES NOT RETURN on success. */
    HAL_FLASH_OB_Launch();

    /* If we get here, something went wrong */
    HAL_FLASH_OB_Lock();
    HAL_FLASH_Lock();
    LOG_ERROR(TAG_OTA, "OB_Launch returned — swap failed");
    return OTA_ERROR_SWAP;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Full OTA Flow (convenience)
 * ═══════════════════════════════════════════════════════════════════════════ */

OTAStatus_t OTA_CheckAndUpdate(uint8_t *ram_buffer, uint32_t ram_size)
{
    OTAVersionInfo_t info;
    OTAStatus_t status;

    /* Step 1: Check */
    status = OTA_CheckForUpdate(&info);
    if (status != OTA_OK)
        return status;  /* OTA_NO_UPDATE or error */

    /* Step 2: Download & Flash */
    LOG_INFO(TAG_OTA, "═══════════════════════════════════════");
    LOG_INFO(TAG_OTA, "  OTA UPDATE: v%s → v%s", FW_VERSION, info.version);
    LOG_INFO(TAG_OTA, "═══════════════════════════════════════");

    status = OTA_DownloadAndFlash(&info, ram_buffer, ram_size);
    if (status != OTA_OK)
    {
        LOG_ERROR(TAG_OTA, "Download/flash failed (err=%d) — aborting update", status);
        return status;
    }

    /* Step 3: Swap & Reset */
    return OTA_SwapBankAndReset();
}

/* Trigger CI update */





