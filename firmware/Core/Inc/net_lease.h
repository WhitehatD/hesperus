/**
 * @file    net_lease.h
 * @brief   Flash-persistent last-known-good IPv4 lease, keyed by SSID.
 *
 * Vendor-neutral replacement for the hardcoded iPhone-hotspot static IP
 * fallback. See net_lease.c for the rationale and RFC reference.
 */
#ifndef __NET_LEASE_H
#define __NET_LEASE_H

#include <stdint.h>

typedef enum {
    LEASE_OK = 0,
    LEASE_EMPTY,        /**< nothing stored, or stored for a different SSID */
    LEASE_CORRUPT,
    LEASE_FLASH_ERROR,
} LeaseStatus_t;

typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
} NetLease_t;

/**
 * @brief Load the cached lease for @p ssid, if one was stored for that SSID.
 * @retval LEASE_OK      lease valid and returned in @p out
 * @retval LEASE_EMPTY   no lease stored for this SSID
 * @retval LEASE_CORRUPT stored block failed magic/CRC validation
 */
LeaseStatus_t NetLease_Load(const char *ssid, NetLease_t *out);

/**
 * @brief Persist @p lease as the last-known-good config for @p ssid.
 *
 * No-ops (returns LEASE_OK) when the stored block already matches, so this
 * is safe to call after every successful DHCP without burning flash cycles.
 */
LeaseStatus_t NetLease_Save(const char *ssid, const NetLease_t *lease);

#endif /* __NET_LEASE_H */
