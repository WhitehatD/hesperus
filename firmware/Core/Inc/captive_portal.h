/**
 * @file    captive_portal.h
 * @brief   WiFi Captive Portal — SoftAP + Embedded HTTP Server
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Starts a WiFi Access Point and serves a local configuration page
 * for setting the WiFi SSID/password. Includes DNS redirect for
 * automatic captive portal detection on mobile devices.
 */

#ifndef __CAPTIVE_PORTAL_H
#define __CAPTIVE_PORTAL_H

#include <stdint.h>
#include <stdbool.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    PORTAL_OK = 0,
    PORTAL_ERROR_AP,        /* SoftAP start failed */
    PORTAL_ERROR_SERVER,    /* TCP server bind/listen failed */
    PORTAL_ERROR_DNS,       /* DNS redirect setup failed */
    PORTAL_CONFIGURED,      /* User submitted credentials — reboot pending */
} PortalStatus_t;

/**
 * @brief  Why the portal was entered — drives which BoardStatus LED
 *         pattern is shown, and whether a background sanity re-check of
 *         the OLD stored credentials runs while the portal is open.
 */
typedef enum {
    PORTAL_REASON_FRESH = 0,     /* No stored credentials at all (first boot) */
    PORTAL_REASON_MANUAL,        /* Operator explicitly requested it (button hold / MQTT command) */
    PORTAL_REASON_CREDS_SUSPECT, /* Auto-opened: AP was confirmed present but repeated auth/DHCP
                                   * failures suggest the stored password is wrong. Distinct LED
                                   * pattern ("not trying to connect, portal is up on purpose") +
                                   * periodic background re-check of the OLD creds in case it was
                                   * a fluke, auto-exiting the portal on success. */
} PortalReason_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Start the captive portal.
 *
 * 1. Starts SoftAP with SSID "IoT-Setup-XXXX" (last 4 MAC hex)
 * 2. Starts DNS redirect (all queries → 192.168.10.1)
 * 3. Starts HTTP server on port 80
 * 4. BLOCKS until user submits WiFi credentials via the config page — OR,
 *    if `reason` is PORTAL_REASON_CREDS_SUSPECT, until a periodic
 *    background re-check of the previously-stored credentials succeeds
 *    (in which case the portal tears itself down and reboots into normal
 *    operation with no user action needed).
 * 5. Saves credentials to flash and triggers system reboot
 *
 * @param  reason: why the portal was entered (see PortalReason_t).
 * @retval PORTAL_CONFIGURED on success (never returns — reboots).
 *         PORTAL_ERROR_* on failure.
 */
PortalStatus_t CaptivePortal_Start(PortalReason_t reason);

/**
 * @brief  Stop the captive portal and tear down SoftAP.
 *         Called internally or externally to abort portal mode.
 */
void CaptivePortal_Stop(void);

#endif /* __CAPTIVE_PORTAL_H */
