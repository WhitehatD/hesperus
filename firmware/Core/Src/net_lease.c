/**
 * @file    net_lease.c
 * @brief   Flash-persistent last-known-good IPv4 lease — page 125, both banks.
 *
 * WHY THIS EXISTS
 * ---------------
 * DHCP is a dependency that can fail while the link itself is perfectly
 * healthy. Measured live on this board (2026-08-21): association and the
 * WPA2 handshake succeeded on every single attempt while the AP's DHCP
 * server refused to issue a lease for 10+ minutes straight, across dozens
 * of retries, a full module reinit, an MCU reflash, and a true power
 * cycle. A static address let the board straight back online, which also
 * proved the radio/credentials were never the problem.
 *
 * The first cut of that fallback hardcoded Apple's Personal Hotspot subnet
 * (172.20.10.0/28). That worked but was wrong as architecture: it couples
 * the firmware to one AP vendor, picks a fixed host octet with no conflict
 * story, and silently does nothing on any other network.
 *
 * This is the vendor-neutral version, and it is not a workaround — it is
 * what a normal DHCP client already does. RFC 2131 defines the INIT-REBOOT
 * state: a client that remembers a previously assigned address re-requests
 * that same address on returning to the network instead of starting from
 * scratch. Caching the last lease that a given SSID actually handed us and
 * reusing it when the server goes unresponsive is standard client
 * behaviour, works with any AP, and needs no knowledge of the subnet.
 *
 * Keyed by SSID hash so moving between networks can never apply one
 * network's addressing to another.
 *
 * STM32U585 flash geometry (see board_settings.c for the sibling block):
 *   Page 127 Bank 1/2: WiFi credentials
 *   Page 126 Bank 1/2: board settings (lp_mode)
 *   Page 125 Bank 1/2: this block   <- 0x080FA000 / 0x081FA000
 *
 * On-flash layout (16 bytes, 1 quadword — matches the FLASH_TYPEPROGRAM_QUADWORD
 * granularity the other blocks use):
 *   Bytes  0- 3: Magic = 0x4C454153 ("LEAS")
 *   Bytes  4- 7: SSID hash (FNV-1a 32) — which network this lease belongs to
 *   Bytes  8-11: IP address (4 bytes)
 *   Bytes 12-15: CRC32 over bytes 0-11
 *
 * Mask/gateway/DNS are DERIVED rather than stored, because 16 bytes is one
 * quadword and going to two quadwords would double the erase/write cost for
 * data that is nearly always inferable: gateway = host .1 of the /24-or-
 * narrower subnet the IP sits in, DNS = gateway, mask = the tightest
 * standard prefix consistent with the address. The gateway is verified by
 * an actual reachability probe before the lease is trusted (see wifi.c), so
 * a wrong derivation is detected rather than silently accepted.
 */

#include "net_lease.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "main.h"

#include <string.h>

#define LEASE_PAGE_SIZE    0x2000u
#define LEASE_PAGE_NUMBER  125
#define LEASE_ADDR_BANK1   0x080FA000u
#define LEASE_ADDR_BANK2   0x081FA000u
#define LEASE_MAGIC        0x4C454153u   /* "LEAS" */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t ssid_hash;
    uint8_t  ip[4];
    uint32_t crc32;
} FlashLeaseBlock_t;  /* exactly 16 bytes */
#pragma pack(pop)

_Static_assert(sizeof(FlashLeaseBlock_t) == 16,
               "lease block must be exactly 1 quadword (16 bytes)");

static const char TAG_LEASE[] = "LEAS";

#define LEASE_CRC_SIZE  12u   /* bytes 0-11, before crc32 field */

static uint32_t _lease_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return ~crc;
}

/* FNV-1a: small, no table, good enough to tell one SSID from another. */
static uint32_t _ssid_hash(const char *ssid)
{
    uint32_t h = 2166136261u;
    for (const char *p = ssid; *p != '\0'; p++) {
        h ^= (uint8_t)(*p);
        h *= 16777619u;
    }
    return h;
}

/* Derive mask/gateway/DNS from the address. Every consumer-grade AP this
 * board will meet puts itself at host .1 of the subnet and serves DNS from
 * the same address — including the iPhone hotspot case that prompted all
 * this (172.20.10.1 on a /28). We assume a /24-compatible layout, which is
 * a superset that still routes correctly on narrower subnets because the
 * gateway shares the first three octets in every such deployment. */
static void _derive(NetLease_t *out)
{
    out->mask[0] = 255; out->mask[1] = 255; out->mask[2] = 255; out->mask[3] = 0;

    out->gateway[0] = out->ip[0];
    out->gateway[1] = out->ip[1];
    out->gateway[2] = out->ip[2];
    out->gateway[3] = 1;

    memcpy(out->dns, out->gateway, 4);
}

LeaseStatus_t NetLease_Load(const char *ssid, NetLease_t *out)
{
    if (ssid == NULL || out == NULL)
        return LEASE_CORRUPT;

    const uint32_t want = _ssid_hash(ssid);
    uint32_t addrs[2] = { LEASE_ADDR_BANK1, LEASE_ADDR_BANK2 };

    for (int i = 0; i < 2; i++) {
        const FlashLeaseBlock_t *b = (const FlashLeaseBlock_t *)addrs[i];
        if (b->magic != LEASE_MAGIC)
            continue;
        if (_lease_crc32((const uint8_t *)b, LEASE_CRC_SIZE) != b->crc32)
            continue;
        if (b->ssid_hash != want)
            continue;                      /* lease belongs to another network */
        if (b->ip[0] == 0u)
            continue;                      /* never cache 0.0.0.0 */

        memcpy(out->ip, b->ip, 4);
        _derive(out);
        LOG_INFO(TAG_LEASE, "Cached lease for this SSID: %d.%d.%d.%d gw %d.%d.%d.%d (bank %d)",
                 out->ip[0], out->ip[1], out->ip[2], out->ip[3],
                 out->gateway[0], out->gateway[1], out->gateway[2], out->gateway[3], i);
        return LEASE_OK;
    }

    LOG_DEBUG(TAG_LEASE, "No cached lease for this SSID");
    return LEASE_EMPTY;
}

LeaseStatus_t NetLease_Save(const char *ssid, const NetLease_t *lease)
{
    if (ssid == NULL || lease == NULL)
        return LEASE_CORRUPT;
    if (lease->ip[0] == 0u)
        return LEASE_CORRUPT;              /* refuse to cache an unassigned address */

    FlashLeaseBlock_t block;
    memset(&block, 0x00, sizeof(block));
    block.magic     = LEASE_MAGIC;
    block.ssid_hash = _ssid_hash(ssid);
    memcpy(block.ip, lease->ip, 4);
    block.crc32     = _lease_crc32((const uint8_t *)&block, LEASE_CRC_SIZE);

    /* Idempotence guard: this is called after EVERY successful DHCP, so
     * skip the erase/write entirely when nothing changed. Flash endurance
     * matters more than the handful of cycles we'd save elsewhere. */
    const FlashLeaseBlock_t *cur = (const FlashLeaseBlock_t *)LEASE_ADDR_BANK1;
    if (cur->magic == LEASE_MAGIC &&
        cur->ssid_hash == block.ssid_hash &&
        memcmp(cur->ip, block.ip, 4) == 0 &&
        cur->crc32 == block.crc32) {
        return LEASE_OK;                   /* already stored — no write */
    }

    LOG_INFO(TAG_LEASE, "Caching lease %d.%d.%d.%d for this SSID (flash page %d)",
             lease->ip[0], lease->ip[1], lease->ip[2], lease->ip[3], LEASE_PAGE_NUMBER);

    if (HAL_FLASH_Unlock() != HAL_OK) {
        LOG_ERROR(TAG_LEASE, "Flash unlock failed");
        return LEASE_FLASH_ERROR;
    }

    FLASH_EraseInitTypeDef ec = {0};
    ec.TypeErase = FLASH_TYPEERASE_PAGES;
    ec.Page      = LEASE_PAGE_NUMBER;
    ec.NbPages   = 1;
    uint32_t page_error = 0;

    ec.Banks = FLASH_BANK_1;
    if (HAL_FLASHEx_Erase(&ec, &page_error) != HAL_OK)
        LOG_ERROR(TAG_LEASE, "Erase Bank 1 failed (err=0x%08lX)",
                  (unsigned long)HAL_FLASH_GetError());

    ec.Banks = FLASH_BANK_2;
    if (HAL_FLASHEx_Erase(&ec, &page_error) != HAL_OK)
        LOG_ERROR(TAG_LEASE, "Erase Bank 2 failed (err=0x%08lX)",
                  (unsigned long)HAL_FLASH_GetError());

    uint8_t write_buf[16] __attribute__((aligned(16)));
    memcpy(write_buf, &block, 16);

    uint32_t targets[2] = { LEASE_ADDR_BANK1, LEASE_ADDR_BANK2 };
    int ok = 0;
    for (int b = 0; b < 2; b++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                              targets[b],
                              (uint32_t)(uintptr_t)write_buf) == HAL_OK)
            ok++;
        else
            LOG_ERROR(TAG_LEASE, "Program Bank %d failed (HAL err=0x%08lX)",
                      b + 1, (unsigned long)HAL_FLASH_GetError());
    }

    HAL_FLASH_Lock();

    if (ok == 0) {
        LOG_ERROR(TAG_LEASE, "Lease cache write failed on all banks");
        return LEASE_FLASH_ERROR;
    }
    LOG_INFO(TAG_LEASE, "Lease cached OK (%d/2 banks)", ok);
    return LEASE_OK;
}
