/**
 * @file    board_status.h
 * @brief   Firmware-wide Board Status LED State Machine
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * The B-U585I-IOT02A has exactly two mono-color LEDs (RED, GREEN) and no
 * display. Every subsystem that used to poke BSP_LED_On/Off directly for
 * connectivity/portal/OTA/idle signalling now goes through this module
 * instead, so the board's on-device state is always legible and never
 * drifts out of sync between call sites.
 *
 * Three-tier model (revised 2026-08-20 after a verification pass found the
 * original single-priority-ratchet design was fundamentally broken — see
 * board_status.c for the post-mortem):
 *   - Tier 1 "phase": BOOTING / ASLEEP / ONLINE_IDLE / MQTT_RECONNECTING /
 *     WIFI_SEARCHING / WIFI_CONNECTING. These describe what the board is
 *     routinely DOING and must transition freely in any direction as
 *     connectivity changes — Set() always applies immediately, no
 *     priority gating. (The original design priority-gated these too,
 *     which meant e.g. ONLINE_IDLE could never be re-applied once
 *     WIFI_CONNECTING had ever been set — a one-way ratchet that
 *     permanently froze the LED after the very first boot connect.)
 *   - Tier 2 "override": OTA_IN_PROGRESS / PORTAL_ACTIVE /
 *     PORTAL_CREDS_SUSPECT / FACTORY_RESET_WARNING / ERROR_FATAL. These
 *     must NOT be silently clobbered by a routine phase transition
 *     happening underneath them, so they use priority-gated Set() +
 *     ownership-based Clear() that restores whatever override (if any)
 *     was active before — overrides only ever nest against other
 *     overrides (the one real case in this firmware: FACTORY_RESET_WARNING
 *     interrupting an active PORTAL_ACTIVE/PORTAL_CREDS_SUSPECT), never
 *     against tier-1 phase states, so a single previous-override slot is
 *     sufficient — no stack needed for this firmware's actual call graph.
 *     Rendering always prefers the active override over the phase.
 *   - Tier 3 "activity pulse": a brief (~200ms) overlay flash for
 *     one-shot events too short-lived to own the LEDs outright (a single
 *     photo capture). Does not change phase or override.
 *
 * Non-blocking: BoardStatus_Tick() must be called every main-loop
 * iteration (or from any tight poll loop, e.g. the boot-time WiFi retry
 * loop) and only ever touches HAL_GetTick() + GPIO — it never sleeps.
 */

#ifndef __BOARD_STATUS_H
#define __BOARD_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* ── Base States (priority-ordered — see board_status.c) ─────────────── */
typedef enum {
    BOARD_STATUS_BOOTING = 0,        /* Cold boot, subsystems not up yet — both LEDs solid */
    BOARD_STATUS_ASLEEP,             /* PS-REST / deep-dormant STOP2 — LEDs off (energy) */
    BOARD_STATUS_ONLINE_IDLE,        /* Connected, MQTT up, nothing pending — GREEN heartbeat blip */
    BOARD_STATUS_MQTT_RECONNECTING,  /* WiFi up, broker unreachable — RED slow blink */
    BOARD_STATUS_WIFI_SEARCHING,     /* Stored SSID not seen in scan — AP presumed absent, retrying
                                       * forever in the background — GREEN slow blip, calm/not urgent */
    BOARD_STATUS_WIFI_CONNECTING,    /* Actively attempting a connect, SSID confirmed present — GREEN fast blink */
    BOARD_STATUS_OTA_IN_PROGRESS,    /* Firmware download/flash in progress — RED/GREEN alternate */
    BOARD_STATUS_PORTAL_ACTIVE,      /* SoftAP + HTTP portal up (fresh device or manual button
                                       * entry) — RED slow blink. A connected/serving HTTP client
                                       * is signalled with a brief GREEN activity pulse layered on
                                       * top (BOARD_PULSE_MQTT_TX-style overlay), not a separate
                                       * base state — see note below on why. */
    BOARD_STATUS_PORTAL_CREDS_SUSPECT, /* Portal auto-opened because stored creds look wrong (AP
                                       * present, repeated auth failure) — RED solid + GREEN slow
                                       * pulse: "not trying to connect, portal is up on purpose" */
    BOARD_STATUS_FACTORY_RESET_WARNING, /* USER button held past the warn threshold — about to
                                       * erase credentials unless released — fast RED/GREEN alternate,
                                       * highest priority so it's ALWAYS visible over any other state */
    BOARD_STATUS_ERROR_FATAL,        /* Unrecoverable init failure — RED fast blink */
    BOARD_STATUS__COUNT
} BoardStatus_t;

/* ── Activity Pulses (Layer 2 — brief overlay, does not change base state) */
typedef enum {
    BOARD_PULSE_CAPTURE = 0,   /* Photo taken */
    BOARD_PULSE_UPLOAD_CHUNK,  /* Upload chunk milestone */
    BOARD_PULSE_MQTT_TX,       /* MQTT publish / command handled */
    BOARD_PULSE_PORTAL_CLIENT, /* Portal HTTP client connected/served — deliberately a pulse, not
                                 * a base state: giving it its own priority level would let it
                                 * silently starve BOARD_STATUS_PORTAL_CREDS_SUSPECT (a client
                                 * mid-request should never make the "not trying to connect,
                                 * creds are suspect" signal disappear). */
} BoardPulse_t;

/**
 * @brief  Initialize the module (LED HW must already be BSP_LED_Init'd).
 */
void BoardStatus_Init(void);

/**
 * @brief  Request a state transition.
 *
 * Tier-1 "phase" states (BOOTING/ASLEEP/ONLINE_IDLE/MQTT_RECONNECTING/
 * WIFI_SEARCHING/WIFI_CONNECTING) always apply immediately — call this
 * freely as connectivity changes.
 *
 * Tier-2 "override" states (OTA_IN_PROGRESS/PORTAL_ACTIVE/PORTAL_CREDS_SUSPECT/
 * FACTORY_RESET_WARNING/ERROR_FATAL) are priority-gated against any
 * currently active override, and rendering always prefers the active
 * override over whatever phase is underneath.
 *
 * @retval true if applied, false only for a tier-2 request that lost to
 *         a higher-priority override already active (tier-1 requests
 *         always return true).
 */
bool BoardStatus_Set(BoardStatus_t state);

/**
 * @brief  Release a tier-2 override ownership claim. Only takes effect
 *         if `state` is the currently active override (ownership-based
 *         — avoids one subsystem accidentally clearing another's).
 *         Restores whatever override (if any) was active immediately
 *         before `state`, or exposes the tier-1 phase underneath if
 *         there was none. No-op for tier-1 states (nothing to clear —
 *         just Set() a different phase instead).
 */
void BoardStatus_Clear(BoardStatus_t state);

/**
 * @brief  Get the currently rendered state (active override, or the
 *         current phase if no override is active).
 */
BoardStatus_t BoardStatus_Get(void);

/**
 * @brief  Fire a brief activity-pulse overlay. Non-blocking — schedules a
 *         short flash rendered by the next few BoardStatus_Tick() calls.
 */
void BoardStatus_Pulse(BoardPulse_t pulse);

/**
 * @brief  Render the current state to the physical LEDs. Call every
 *         main-loop iteration (and from any blocking-ish retry/backoff
 *         wait loop, e.g. boot-time WiFi retry) — never blocks.
 */
void BoardStatus_Tick(void);

#endif /* __BOARD_STATUS_H */
