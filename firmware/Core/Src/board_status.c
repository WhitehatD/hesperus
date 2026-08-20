/**
 * @file    board_status.c
 * @brief   Firmware-wide Board Status LED State Machine
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * See board_status.h for the architecture note. This file owns the only
 * BSP_LED_On/Off/Toggle call sites for connectivity/portal/OTA/idle
 * signalling in the firmware — everything else routes through
 * BoardStatus_Set()/Pulse()/Tick().
 */

#include "board_status.h"
#include "main.h"
#include "firmware_config.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Pattern Table — one entry per base state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK,
} LedMode_t;

typedef struct {
    uint8_t  priority;      /* Higher wins — see BoardStatus_Set() */
    LedMode_t red_mode;
    LedMode_t green_mode;
    uint32_t period_ms;     /* Blink period (shared by both LEDs unless alternate) */
    uint32_t duty_ms;       /* On-time within the period, for whichever LED(s) blink */
    uint8_t  alternate;     /* 1 = red/green blink out-of-phase (half-period offset) */
} BoardStatusPattern_t;

/* clang-format off */
static const BoardStatusPattern_t s_patterns[BOARD_STATUS__COUNT] = {
    [BOARD_STATUS_BOOTING]                = { 10, LED_MODE_ON,    LED_MODE_ON,    0,    0,   0 },
    [BOARD_STATUS_ASLEEP]                 = { 5,  LED_MODE_OFF,   LED_MODE_OFF,   0,    0,   0 },
    [BOARD_STATUS_ONLINE_IDLE]            = { 20, LED_MODE_OFF,   LED_MODE_BLINK, IDLE_BLINK_PERIOD_MS, IDLE_BLINK_ON_MS, 0 },
    [BOARD_STATUS_MQTT_RECONNECTING]      = { 40, LED_MODE_BLINK, LED_MODE_OFF,   1000, 500, 0 },
    [BOARD_STATUS_WIFI_SEARCHING]         = { 45, LED_MODE_OFF,   LED_MODE_BLINK, 3000, 120, 0 },
    [BOARD_STATUS_WIFI_CONNECTING]        = { 50, LED_MODE_OFF,   LED_MODE_BLINK, 200,  100, 0 },
    [BOARD_STATUS_OTA_IN_PROGRESS]        = { 70, LED_MODE_BLINK, LED_MODE_BLINK, 250,  125, 1 },
    [BOARD_STATUS_PORTAL_ACTIVE]          = { 75, LED_MODE_BLINK, LED_MODE_OFF,   1000, 500, 0 },
    [BOARD_STATUS_PORTAL_CREDS_SUSPECT]   = { 90, LED_MODE_ON,    LED_MODE_BLINK, 2000, 150, 0 },
    [BOARD_STATUS_FACTORY_RESET_WARNING]  = { 95, LED_MODE_BLINK, LED_MODE_BLINK, 300,  150, 1 },
    [BOARD_STATUS_ERROR_FATAL]            = { 100, LED_MODE_BLINK, LED_MODE_OFF,  200,  100, 0 },
};
/* clang-format on */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Module State — see the two-tier phase/override split below
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Activity pulse — a single active pulse at a time is plenty for a 2-LED
 * board; a newer pulse simply replaces an in-flight one. */
static volatile uint8_t  s_pulse_active = 0;
static volatile uint32_t s_pulse_start_tick = 0;
#define PULSE_DURATION_MS  180u

/* ═══════════════════════════════════════════════════════════════════════════
 *  POST-MORTEM (2026-08-20): the original implementation here was a single
 *  flat priority ratchet — Set(state) applied only if its priority was >=
 *  whatever was currently showing, full stop. That is correct for a small
 *  set of true overrides, but this module also has "phase" states that
 *  must transition freely both up AND down as connectivity changes
 *  (WIFI_CONNECTING <-> WIFI_SEARCHING <-> ONLINE_IDLE <-> MQTT_RECONNECTING
 *  <-> ASLEEP). A flat ratchet made every one of those a one-way trip: once
 *  WIFI_CONNECTING (priority 50) had ever been set — which happens on
 *  every single boot — ONLINE_IDLE (20) could never be re-applied, WiFi
 *  going idle after connecting would show "still connecting" forever,
 *  MQTT_RECONNECTING never cleared after recovery, and ASLEEP (5) lost to
 *  the initial BOOTING (10) before the board had even touched WiFi. A
 *  verification pass caught this from the compiled priority table alone,
 *  no hardware needed — see thesis project history for the full report.
 *
 *  Fix: split into two tiers (see board_status.h for the full rationale).
 *  Phase states always apply immediately, no gating. Override states are
 *  priority-gated against each other only, and rendering always prefers
 *  the active override over the phase underneath. */

static inline bool _is_override(BoardStatus_t s)
{
    return s == BOARD_STATUS_OTA_IN_PROGRESS
        || s == BOARD_STATUS_PORTAL_ACTIVE
        || s == BOARD_STATUS_PORTAL_CREDS_SUSPECT
        || s == BOARD_STATUS_FACTORY_RESET_WARNING
        || s == BOARD_STATUS_ERROR_FATAL;
}

#define OVERRIDE_NONE BOARD_STATUS__COUNT

static BoardStatus_t s_phase         = BOARD_STATUS_BOOTING;  /* Tier 1 — free transitions */
static BoardStatus_t s_override      = OVERRIDE_NONE;         /* Tier 2 — priority-gated */
static BoardStatus_t s_override_prev = OVERRIDE_NONE;         /* One level of override nesting
                                                                 * — sufficient because the only
                                                                 * real nesting case in this
                                                                 * firmware is FACTORY_RESET_WARNING
                                                                 * interrupting an active portal. */

void BoardStatus_Init(void)
{
    s_phase = BOARD_STATUS_BOOTING;
    s_override = OVERRIDE_NONE;
    s_override_prev = OVERRIDE_NONE;
    s_pulse_active = 0;
}

bool BoardStatus_Set(BoardStatus_t state)
{
    if ((uint32_t)state >= BOARD_STATUS__COUNT)
        return false;

    if (!_is_override(state))
    {
        s_phase = state;  /* Tier 1: always applies, no priority gating */
        return true;
    }

    /* Tier 2: gate against whatever override (if any) is currently active */
    if (s_override != OVERRIDE_NONE &&
        s_patterns[state].priority < s_patterns[s_override].priority)
        return false;  /* A more urgent override is already showing */

    if (state != s_override)
    {
        s_override_prev = s_override;
        s_override = state;
    }
    return true;
}

void BoardStatus_Clear(BoardStatus_t state)
{
    /* Ownership-based: only the subsystem that owns the currently active
     * override may clear it. Restores whatever override (if any) was
     * active immediately before — e.g. aborting a FACTORY_RESET_WARNING
     * that interrupted an active portal correctly goes back to the
     * portal, not to the tier-1 phase underneath both of them. No-op for
     * tier-1 states — there's nothing to "clear" about a phase, just
     * Set() a different one. */
    if (!_is_override(state))
        return;

    if (s_override == state)
    {
        s_override = s_override_prev;
        s_override_prev = OVERRIDE_NONE;
    }
    else if (s_override_prev == state)
    {
        /* The owner is releasing a state that is currently PARKED behind a
         * higher-priority override (e.g. CaptivePortal_Stop() runs while a
         * FACTORY_RESET_WARNING is on top of PORTAL_ACTIVE). Purge it from
         * the parked slot too — otherwise clearing the outer override later
         * would resurrect a portal state whose owner already shut down.
         * Not reachable in today's call graph (the portal's blocking loop
         * never polls the button gesture), but the ownership contract must
         * hold regardless of who calls what in future. */
        s_override_prev = OVERRIDE_NONE;
    }
}

BoardStatus_t BoardStatus_Get(void)
{
    return (s_override != OVERRIDE_NONE) ? s_override : s_phase;
}

void BoardStatus_Pulse(BoardPulse_t pulse)
{
    (void)pulse;  /* Only one visual pulse shape today — kept for future
                    * per-kind differentiation (e.g. double-blip for upload
                    * vs single for capture) without an API change. */
    s_pulse_active = 1;
    s_pulse_start_tick = HAL_GetTick();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Rendering
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t _led_phase_on(uint32_t now, uint32_t period_ms, uint32_t duty_ms,
                                     uint32_t phase_offset_ms)
{
    if (period_ms == 0) return 0;
    uint32_t pos = (now + phase_offset_ms) % period_ms;
    return (pos < duty_ms) ? 1 : 0;
}

void BoardStatus_Tick(void)
{
    const BoardStatusPattern_t *p = &s_patterns[BoardStatus_Get()];
    uint32_t now = HAL_GetTick();

    /* An in-flight activity pulse briefly forces GREEN on regardless of the
     * base pattern (it never fights RED, which is reserved for base-state
     * urgency signalling) — capture/upload/MQTT-tx blips stay visible even
     * while e.g. ONLINE_IDLE would otherwise have GREEN off. */
    if (s_pulse_active)
    {
        if ((now - s_pulse_start_tick) < PULSE_DURATION_MS)
        {
            BSP_LED_On(LED_GREEN);
        }
        else
        {
            s_pulse_active = 0;
        }
    }

    /* RED */
    switch (p->red_mode)
    {
        case LED_MODE_ON:    BSP_LED_On(LED_RED);  break;
        case LED_MODE_OFF:   BSP_LED_Off(LED_RED); break;
        case LED_MODE_BLINK:
            if (_led_phase_on(now, p->period_ms, p->duty_ms, 0))
                BSP_LED_On(LED_RED);
            else
                BSP_LED_Off(LED_RED);
            break;
    }

    /* GREEN — skip while a pulse owns it, unless the base state itself
     * wants GREEN driven (pulse already turned it on above; blink/off
     * modes still need to resolve once the pulse window ends). */
    if (!s_pulse_active)
    {
        uint32_t green_phase_offset = p->alternate ? (p->period_ms / 2) : 0;
        switch (p->green_mode)
        {
            case LED_MODE_ON:    BSP_LED_On(LED_GREEN);  break;
            case LED_MODE_OFF:   BSP_LED_Off(LED_GREEN); break;
            case LED_MODE_BLINK:
                if (_led_phase_on(now, p->period_ms, p->duty_ms, green_phase_offset))
                    BSP_LED_On(LED_GREEN);
                else
                    BSP_LED_Off(LED_GREEN);
                break;
        }
    }
}
