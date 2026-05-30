/**
  ******************************************************************************
  * @file    main.c
  * @brief   Autonomous IoT Visual Monitoring — Main Application
  * @author  Alexandru-Ionut Cioc (based on ST MCD template)
  * @date    2026
  *
  * Application flow:
  *   BOOT → Init (HAL, Clock, Cache, UART, RTC, Wi-Fi, Camera, MQTT)
  *   → Subscribe to schedule commands via MQTT
  *   → Poll MQTT until a schedule arrives from the AI planning engine
  *   → For each scheduled task:
  *       1. Set RTC alarm → Enter STOP2 low-power mode
  *       2. Wake on alarm → Capture image with camera
  *       3. Upload image to server via HTTP POST
  *       4. Publish status update via MQTT
  *       5. Repeat until all tasks complete
  *   → Enter deep sleep / wait for next schedule
  ******************************************************************************
  * @note    Added comment to trigger GitHub Actions CI/CD pipeline
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "wifi.h"
#include "app_camera.h"
#include "mqtt_handler.h"
#include "scheduler.h"
#include "ota_update.h"
#include "wifi_credentials.h"
#include "board_settings.h"
#include "captive_portal.h"
#include "upload_async.h"
#include "mx_wifi.h"

#include "cJSON.h"
#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Global Handles (shared with ISR and other modules)
 * ═══════════════════════════════════════════════════════════════════════════ */

RTC_HandleTypeDef hrtc;

/* SEC-07: Independent Watchdog for autonomous recovery */
#if WATCHDOG_ENABLED
static IWDG_HandleTypeDef hiwdg;
#endif

/* ── Reset Reason Reporting ─────────────────────────────────────────── */
static void Boot_ReportResetReason(void)
{
    uint32_t rsr = RCC->CSR;
    LOG_INFO("BOOT", "Reset Reason Register (CSR): 0x%08lX", rsr);

    if (rsr & RCC_CSR_IWDGRSTF) LOG_WARN("BOOT", "Reset caused by Independent Watchdog (IWDG)");
    if (rsr & RCC_CSR_WWDGRSTF) LOG_WARN("BOOT", "Reset caused by Window Watchdog (WWDG)");
    if (rsr & RCC_CSR_SFTRSTF)  LOG_INFO("BOOT", "Reset caused by Software (NVIC_SystemReset)");
    if (rsr & RCC_CSR_BORRSTF)  LOG_WARN("BOOT", "Reset caused by Brown-out (BOR)");
    if (rsr & RCC_CSR_PINRSTF)  LOG_INFO("BOOT", "Reset caused by External Pin (NRST)");
    if (rsr & RCC_CSR_LPWRRSTF) LOG_WARN("BOOT", "Reset caused by Illegal Low Power Entry");

    /* Clear reset flags for next boot */
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Private Variables
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Image capture buffer — 32-byte aligned for DCMI DMA */
static uint8_t s_image_buffer[CAMERA_FRAME_BUFFER_SIZE] __attribute__((aligned(32)));

/* Active schedule */
static Schedule_t s_schedule;

/* Flag: set when a new schedule is received via MQTT */
static volatile uint8_t s_schedule_received = 0;

/* B3 USER button — short press = capture, long press (≥3s) = portal mode.
 * ISR records press timestamp; main loop polls pin to detect release. */
static volatile uint32_t s_button_press_tick = 0;
static volatile uint8_t  s_button_held = 0;       /* 1 = button currently held */
static uint8_t           s_button_led_on = 0;      /* Tracks LED feedback state */
static uint32_t s_button_task_id = 10000;  /* Start at high IDs to avoid schedule collision */

/* ── Capture Now Queue (MQTT command) ────────────────────── */
/* Ring buffer so rapid-fire agent commands never drop requests.
 * MQTT callback enqueues, main loop dequeues one per iteration. */
#define CAPTURE_QUEUE_SIZE 16
static volatile uint32_t s_capture_queue[CAPTURE_QUEUE_SIZE];
static volatile uint8_t  s_capture_queue_head = 0;  /* Next write slot (producer: MQTT callback) */
static volatile uint8_t  s_capture_queue_tail = 0;  /* Next read slot  (consumer: main loop) */

static inline uint8_t _capture_queue_count(void) {
    return (uint8_t)((s_capture_queue_head - s_capture_queue_tail) % CAPTURE_QUEUE_SIZE);
}
static inline int _capture_queue_full(void) {
    return _capture_queue_count() == (CAPTURE_QUEUE_SIZE - 1);
}
static inline int _capture_queue_empty(void) {
    return s_capture_queue_head == s_capture_queue_tail;
}

/* Legacy aliases for code that still uses the old flag pattern */
static volatile uint8_t  s_capture_now_requested = 0;
static volatile uint32_t s_capture_now_task_id   = 20000;

/* ── Capture Sequence (MQTT command) ──────────────────────── */
#define MAX_SEQUENCE_CAPTURES 16
static volatile uint8_t  s_sequence_requested = 0;
static volatile uint8_t  s_sequence_abort     = 0;  /* Set by delete_schedule to stop running sequence */
static volatile uint32_t s_sequence_count     = 0;
static uint32_t s_sequence_delays_ms[MAX_SEQUENCE_CAPTURES];  /* ms offset from NOW for each capture */
static uint32_t s_sequence_base_task_id = 30000;

/* ── Sleep mode toggle (MQTT command) ───────────────────── */
static volatile uint8_t s_sleep_enabled = 0;  /* 0 = stay awake (agent controls sleep via sleep_mode MQTT) */

/* ── PS-REST low-power mode (MQTT command) ──────────────── */
/* 0 = always-awake (default; no behaviour change).
 * 1 = PS-REST: STOP2 + WiFi associated in 802.11 power-save.
 *     Board polls MQTT every REST_POLL_S seconds; agent-controllable
 *     at any time via EXTI/MQTT wake or B3 button override. */
static volatile uint8_t s_lp_mode = 0;
/* ── Energy phase-timer (task #51) ─────────────────────────────────────────
 * Accumulate ms in each power state per 60 s window; publish via MQTT when
 * PS-REST is active. scripts/energy_model.py uses this for duty-cycle energy
 * modelling (RQ3). All three counters reset after each publish. */
static uint32_t s_energy_capture_ms = 0; /* time in active capture this window (awake-tick) */
static uint32_t s_energy_report_ms  = 0; /* HAL_GetTick() at last energy publish (awake-tick anchor) */
static uint32_t s_energy_wall_start = 0; /* RTC secs-of-day at window start (wall-clock anchor) */

/* ── OTA firmware update (MQTT command) ─────────────────── */
static volatile uint8_t s_ota_requested = 0;
static volatile uint8_t s_ota_in_progress = 0;  /* Lock for s_image_buffer */

/* ── Captive Portal (MQTT command or boot fallback) ──── */
static volatile uint8_t s_portal_requested = 0;

/* ── Ping (MQTT command) ────────────────────────────────── */
static volatile uint8_t s_ping_requested = 0;

/* ── Runtime WiFi credential management (MQTT command) ──── */
static volatile uint8_t s_wifi_reconfig_requested = 0;
static char s_runtime_ssid[33]     = {0};  /* Max 32-char SSID + null */
static char s_runtime_password[64] = {0};  /* Max 63-char WPA2 + null */
static uint8_t s_has_runtime_wifi  = 0;    /* 1 = runtime creds set   */

/* OBS-02: Global Telemetry */
typedef struct {
    uint32_t wifi_reconnects;
    uint32_t mqtt_reconnects;
    uint32_t capture_failures;
    uint32_t ota_checks;
    uint32_t ota_failures;
} FirmwareTelemetry_t;
static FirmwareTelemetry_t s_telemetry = {0};

/* Buffer to hold the raw JSON schedule from MQTT callback */
static char s_schedule_json[SCHEDULE_JSON_MAX];

/* ═══════════════════════════════════════════════════════════════════════════
 *  Private Function Prototypes
 * ═══════════════════════════════════════════════════════════════════════════ */

void SystemClock_Config(void);
static void CACHE_Enable(void);
static void RTC_Init(void);
static void LED_SignalError(void);
static void on_command_received(const char *json_str, uint32_t length);
static void _do_button_capture(void);
static void _do_capture_now(void);
static void _do_capture_sequence(void);
static void _do_ota_update(void);
static void _do_wifi_reconfig(void);
static void _do_ping_sequence(void);
static void _do_start_portal(void);
static void EnterPSRest(void);

/* SEC-07: Watchdog initialization */
#if WATCHDOG_ENABLED
static void Watchdog_Init(void);
static void Watchdog_FreezeInStop(void);
#endif
void Watchdog_Refresh(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEC-11: cJSON Static Bump Allocator (Fintech Heap Protection)
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint8_t s_json_mem[12288];  /* 12 KB dedicated static pool for JSON DOM */
static size_t s_json_idx = 0;

static void* json_malloc(size_t sz) {
    size_t align = (sz + 7) & ~7;
    if(s_json_idx + align > sizeof(s_json_mem)) return NULL;
    void* p = &s_json_mem[s_json_idx];
    s_json_idx += align;
    return p;
}
static void json_free(void* ptr) { (void)ptr; }

void json_mem_reset(void) {
    s_json_idx = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Stack Watermark — Enterprise RAM Safety
 *
 *  Paints the stack region (top of RAM, grows downward) with canary words.
 *  RAM_CheckStackHighWater() scans from the stack bottom upward to find
 *  the deepest canary-overwrite — that's the peak stack usage.
 *
 *  Memory layout (ARM Cortex-M, stack grows DOWN):
 *    _ebss ──────── end of BSS (heap grows UP from here)
 *    ...heap...
 *    ...free gap...
 *    stack bottom ─ deepest the stack has reached
 *    ...stack...
 *    _estack ────── top of RAM (SP starts here at reset)
 * ═══════════════════════════════════════════════════════════════════════════ */

#if STACK_WATERMARK_ENABLED

/* Linker-defined symbols */
extern uint32_t _estack;   /* Top of stack (highest address, SP at reset) */
extern uint32_t _ebss;     /* End of BSS (heap starts here) */

#define STACK_CANARY_WORD  0xDEADBEEFUL

/* Reserve stack space to paint: from (_estack - STACK_PAINT_SIZE) to _estack.
 * We paint the LOWER portion of the stack region. The stack grows down from
 * _estack, so untouched canaries at the bottom = free stack space. */
#define STACK_PAINT_SIZE   (8 * 1024)  /* Paint 8KB below _estack */

/**
 * @brief  Paint the lower portion of the stack with canary words.
 *         Must be called very early in main(), before deep call chains.
 */
static void RAM_PaintStackCanary(void)
{
    volatile uint32_t *stack_top = (volatile uint32_t *)&_estack;
    volatile uint32_t *paint_start = (volatile uint32_t *)((uint32_t)stack_top - STACK_PAINT_SIZE);

    /* Don't paint into BSS — clamp to _ebss if stack region overlaps */
    volatile uint32_t *bss_end = (volatile uint32_t *)&_ebss;
    if (paint_start < bss_end)
        paint_start = bss_end;

    /* Don't paint over our own current stack frame */
    volatile uint32_t stack_var;
    volatile uint32_t *safe_limit = (volatile uint32_t *)((uint32_t)&stack_var - 256);

    uint32_t painted = 0;
    for (volatile uint32_t *p = paint_start; p < safe_limit && p < stack_top; p++)
    {
        *p = STACK_CANARY_WORD;
        painted++;
    }

    LOG_INFO(TAG_BOOT, "Stack canary: painted %lu words (%lu bytes) below _estack",
             (unsigned long)painted, (unsigned long)(painted * 4));
}

/**
 * @brief  Scan stack region from bottom upward to find peak usage.
 *
 *         ARM stack grows DOWN from _estack. Canaries were placed at the
 *         bottom of the stack region. The first intact canary (scanning up
 *         from paint_start) = boundary of maximum stack depth.
 *
 * @retval Number of bytes used at peak.
 */
static uint32_t RAM_CheckStackHighWater(void)
{
    volatile uint32_t *stack_top = (volatile uint32_t *)&_estack;
    volatile uint32_t *paint_start = (volatile uint32_t *)((uint32_t)stack_top - STACK_PAINT_SIZE);

    volatile uint32_t *bss_end = (volatile uint32_t *)&_ebss;
    if (paint_start < bss_end)
        paint_start = bss_end;

    /* Scan upward from paint_start: find first intact canary */
    volatile uint32_t *p = paint_start;
    while (p < stack_top && *p != STACK_CANARY_WORD)
    {
        p++;
    }

    /* Everything from p to stack_top is free (still has canaries) */
    uint32_t free_bytes = (uint32_t)((uint8_t *)stack_top - (uint8_t *)p);
    uint32_t total_bytes = STACK_PAINT_SIZE;
    uint32_t used_bytes = total_bytes - free_bytes;

    LOG_INFO(TAG_BOOT, "Stack watermark: %lu/%lu bytes used (%lu%% free)",
             (unsigned long)used_bytes,
             (unsigned long)total_bytes,
             (unsigned long)((free_bytes * 100) / total_bytes));

    if (free_bytes < 1024)
    {
        LOG_ERROR(TAG_BOOT, "CRITICAL: Stack nearly exhausted! Only %lu bytes free",
                  (unsigned long)free_bytes);
    }
    else if (free_bytes < 2048)
    {
        LOG_WARN(TAG_BOOT, "WARNING: Stack getting low — %lu bytes free",
                 (unsigned long)free_bytes);
    }

    return used_bytes;
}

#endif /* STACK_WATERMARK_ENABLED */

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT Command Callback — Dispatches by "type" field
 *
 *  Expected JSON formats:
 *    {"type":"capture_now"}
 *    {"type":"capture_sequence","delays_ms":[0, 2000, 5000]}
 *    {"type":"schedule","tasks":[...]}   (legacy: no type = schedule)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void on_command_received(const char *json_str, uint32_t length)
{
    /* Reset JSON arena before parsing new command to prevent fragmentation */
    json_mem_reset();

    /* REL-04: Do not allocate 2KB on stack in ISR/MQTT callback context */
    static char s_cmd_buf[SCHEDULE_JSON_MAX];

    if (length >= SCHEDULE_JSON_MAX)
    {
        LOG_ERROR(TAG_MQTT, "Command too large (%lu > %d), dropping",
                  (unsigned long)length, SCHEDULE_JSON_MAX);
        return;
    }

    /* Null-terminate for cJSON */
    memcpy(s_cmd_buf, json_str, length);
    s_cmd_buf[length] = '\0';

    LOG_INFO(TAG_MQTT, "Command received (%lu bytes)", (unsigned long)length);

    /* Acknowledgement flash */
    BSP_LED_On(LED_GREEN);
    BSP_LED_On(LED_RED);
    HAL_Delay(50);
    BSP_LED_Off(LED_GREEN);
    BSP_LED_Off(LED_RED);

    /* Quick-parse type field with cJSON */
    cJSON *root = cJSON_Parse(s_cmd_buf);
    if (root == NULL)
    {
        LOG_ERROR(TAG_MQTT, "Command JSON parse error");
        return;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    const char *type_str = (type_obj && cJSON_IsString(type_obj)) ? type_obj->valuestring : NULL;
    
    /* ── Fetch explicit task_id from server ── */
    cJSON *task_id_obj = cJSON_GetObjectItem(root, "task_id");
    uint32_t server_task_id = 0;
    int has_server_task_id = 0;
    if (task_id_obj && cJSON_IsNumber(task_id_obj)) {
        server_task_id = (uint32_t)task_id_obj->valuedouble;
        has_server_task_id = 1;
    }

    /* ── Dispatch by command type ───────────────────── */

    if (type_str != NULL && strcmp(type_str, "capture_now") == 0)
    {
        /* Rate limiting is handled by the queue depth (max 16 pending).
         * No time-based throttle — the agent must be able to queue
         * multiple captures in rapid succession. */

        /* ── Enqueue capture request (ring buffer — never drops) ── */
        if (has_server_task_id) {
            s_capture_now_task_id = server_task_id;
        } else {
            s_capture_now_task_id++;
        }

        if (_capture_queue_full()) {
            LOG_WARN(TAG_MQTT, ">> CAPTURE_NOW queue full (%d pending), dropping oldest",
                     CAPTURE_QUEUE_SIZE - 1);
            s_capture_queue_tail = (s_capture_queue_tail + 1) % CAPTURE_QUEUE_SIZE;
        }
        s_capture_queue[s_capture_queue_head] = s_capture_now_task_id;
        s_capture_queue_head = (s_capture_queue_head + 1) % CAPTURE_QUEUE_SIZE;

        LOG_INFO(TAG_MQTT, ">> CAPTURE_NOW enqueued (task_id=%lu, queue=%d)",
                 (unsigned long)s_capture_now_task_id, _capture_queue_count());
    }
    else if (type_str != NULL && strcmp(type_str, "capture_sequence") == 0)
    {
        /* ── Timed capture sequence ── array of ms delays from NOW ── */
        cJSON *delays = cJSON_GetObjectItem(root, "delays_ms");
        if (delays == NULL || !cJSON_IsArray(delays))
        {
            LOG_ERROR(TAG_MQTT, "capture_sequence missing 'delays_ms' array");
            cJSON_Delete(root);
            return;
        }

        int count = cJSON_GetArraySize(delays);
        if (count > MAX_SEQUENCE_CAPTURES)
        {
            LOG_WARN(TAG_MQTT, "Sequence has %d items, clamping to %d",
                     count, MAX_SEQUENCE_CAPTURES);
            count = MAX_SEQUENCE_CAPTURES;
        }

        for (int i = 0; i < count; i++)
        {
            cJSON *item = cJSON_GetArrayItem(delays, i);
            s_sequence_delays_ms[i] = (item && cJSON_IsNumber(item))
                                    ? (uint32_t)item->valuedouble : 0;
        }

        s_sequence_count = (uint32_t)count;
        if (has_server_task_id) {
            s_sequence_base_task_id = server_task_id;
        } else {
            s_sequence_base_task_id += (uint32_t)count;
        }
        s_sequence_requested = 1;

        LOG_INFO(TAG_MQTT, ">> CAPTURE_SEQUENCE command (%d captures)", count);
        for (int i = 0; i < count; i++)
        {
            LOG_DEBUG(TAG_MQTT, "   [%d] delay=%lums", i,
                      (unsigned long)s_sequence_delays_ms[i]);
        }
    }
    else if (type_str != NULL && strcmp(type_str, "sleep_mode") == 0)
    {
        /* ── Sleep mode toggle ── */
        cJSON *enabled_obj = cJSON_GetObjectItem(root, "enabled");
        if (enabled_obj && cJSON_IsBool(enabled_obj))
        {
            s_sleep_enabled = cJSON_IsTrue(enabled_obj) ? 1 : 0;
            if (s_sleep_enabled)
            {
                /* Deep-dormant and PS-REST are mutually exclusive power modes —
                 * enabling Sleep must clear PS-REST or the PS-REST gate would
                 * keep the board in light sleep instead of going dormant. */
                s_lp_mode = 0;
            }
            LOG_INFO(TAG_MQTT, ">> SLEEP_MODE %s", s_sleep_enabled ? "ENABLED" : "DISABLED");
        }
    }
    else if (type_str != NULL && strcmp(type_str, "low_power_mode") == 0)
    {
        /* ── PS-REST low-power mode toggle ──
         *   {"type":"low_power_mode","mode":"ps_rest"} → STOP2+WiFi-PS poll loop
         *   {"type":"low_power_mode","mode":"off"}     → always-awake (default)    */
        cJSON *mode_obj = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const char *mode_label = "off";
        if (mode_obj && cJSON_IsString(mode_obj) && mode_obj->valuestring != NULL)
        {
            if (strcmp(mode_obj->valuestring, "ps_rest") == 0)
            {
                s_lp_mode  = 1;
                s_sleep_enabled = 0;  /* exclusivity: PS-REST clears deep-sleep */
                BoardSettings_SaveLPMode(1u);
                mode_label = "ps_rest";
                /* Reset energy-window anchors so the first PS-REST window
                 * starts clean (not contaminated by prior always-awake time). */
                {
                    RTC_TimeTypeDef _lp_t;
                    RTC_DateTypeDef _lp_d;
                    HAL_RTC_GetTime(&hrtc, &_lp_t, RTC_FORMAT_BIN);
                    HAL_RTC_GetDate(&hrtc, &_lp_d, RTC_FORMAT_BIN);
                    s_energy_wall_start = (uint32_t)_lp_t.Hours * 3600u
                                        + (uint32_t)_lp_t.Minutes * 60u
                                        + (uint32_t)_lp_t.Seconds;
                    s_energy_report_ms  = HAL_GetTick();
                    s_energy_capture_ms = 0;
                }
            }
            else
            {
                s_lp_mode  = 0;
                mode_label = "off";
                BoardSettings_SaveLPMode(0u);
            }
        }
        LOG_INFO(TAG_MQTT, ">> LOW_POWER_MODE %s", mode_label);
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"lp_mode\",\"mode\":\"%s\"}", mode_label);
        MQTT_PublishStatus(status_msg);
    }
    else if (type_str != NULL && strcmp(type_str, "delete_schedule") == 0)
    {
        /* ── Delete / clear active schedule ── */
        memset(&s_schedule, 0, sizeof(s_schedule));
        s_schedule_received = 0;
        s_sequence_abort = 1;  /* Stop any running capture_sequence */
        LOG_INFO(TAG_MQTT, ">> DELETE_SCHEDULE — schedule cleared");
        MQTT_PublishStatus("{\"status\":\"schedule_cleared\"}");
    }
    else if (type_str != NULL && strcmp(type_str, "firmware_update") == 0)
    {
        /* ── OTA firmware update — execute in main loop ── */
        s_ota_requested = 1;
        LOG_INFO(TAG_MQTT, ">> FIRMWARE_UPDATE command received");
        MQTT_PublishStatus("{\"status\":\"ota_update_queued\"}");
    }
    else if (type_str != NULL && strcmp(type_str, "ping") == 0)
    {
        /* ── Ping LED sequence ── */
        s_ping_requested = 1;
        LOG_INFO(TAG_MQTT, ">> PING command received");
        MQTT_PublishStatus("{\"status\":\"ping_received\"}");
    }
    else if (type_str != NULL && strcmp(type_str, "start_portal") == 0)
    {
        /* ── Start captive portal for WiFi reconfiguration ── */
        s_portal_requested = 1;
        LOG_INFO(TAG_MQTT, ">> START_PORTAL command received");
        MQTT_PublishStatus("{\"status\":\"portal_starting\"}");
    }
    else if (type_str != NULL && strcmp(type_str, "erase_wifi") == 0)
    {
        /* ── Erase stored WiFi credentials and reboot into portal ── */
        LOG_INFO(TAG_MQTT, ">> ERASE_WIFI command received");
        MQTT_PublishStatus("{\"status\":\"wifi_credentials_erasing\"}");
        WiFiCred_Erase();
        HAL_Delay(500);
        NVIC_SystemReset();
    }
    else if (type_str != NULL && strcmp(type_str, "set_wifi") == 0)
    {
        /* ── Remote WiFi credential reconfiguration ── */
        cJSON *ssid_obj = cJSON_GetObjectItem(root, "ssid");
        cJSON *pass_obj = cJSON_GetObjectItem(root, "password");

        if (ssid_obj && cJSON_IsString(ssid_obj) &&
            pass_obj && cJSON_IsString(pass_obj) &&
            strlen(ssid_obj->valuestring) > 0 &&
            strlen(ssid_obj->valuestring) <= 32 &&
            strlen(pass_obj->valuestring) >= 8 &&
            strlen(pass_obj->valuestring) <= 63)
        {
            /* Save to flash for persistence across reboots */
            WiFiCred_Save(ssid_obj->valuestring, pass_obj->valuestring);

            strncpy(s_runtime_ssid, ssid_obj->valuestring, sizeof(s_runtime_ssid) - 1);
            s_runtime_ssid[sizeof(s_runtime_ssid) - 1] = '\0';
            strncpy(s_runtime_password, pass_obj->valuestring, sizeof(s_runtime_password) - 1);
            s_runtime_password[sizeof(s_runtime_password) - 1] = '\0';

            s_has_runtime_wifi = 1;
            s_wifi_reconfig_requested = 1;
            LOG_INFO(TAG_MQTT, ">> SET_WIFI command — SSID='%s' (saved to flash + queued)",
                     s_runtime_ssid);
            MQTT_PublishStatus("{\"status\":\"wifi_reconfig_queued\"}");
        }
        else
        {
            LOG_ERROR(TAG_MQTT, ">> SET_WIFI — invalid SSID/password fields");
            MQTT_PublishStatus("{\"status\":\"error\",\"reason\":\"invalid_wifi_credentials\"}");
        }
    }
    else
    {
        /* ── Schedule (default / legacy) ── parse inline, stay in MQTT loop ── */
        memcpy(s_schedule_json, s_cmd_buf, length + 1);
        if (Scheduler_ParseJSON(&s_schedule, s_schedule_json) == 0)
        {
            s_schedule_received = 1;
            LOG_INFO(TAG_MQTT, ">> SCHEDULE command — %d tasks loaded", s_schedule.task_count);
            
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg), 
                     "{\"status\":\"schedule_received\",\"tasks\":%d}", s_schedule.task_count);
            MQTT_PublishStatus(status_msg);
        }
        else
        {
            LOG_ERROR(TAG_MQTT, ">> SCHEDULE command — parse failed");
            MQTT_PublishStatus("{\"status\":\"error\",\"reason\":\"schedule_parse_failed\"}");
        }
    }

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Main Program
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /* ── Phase 1: Hardware Initialization ────────────── */

    HAL_Init();
    CACHE_Enable();
    SystemClock_Config();

    /* SEC-11: Initialize static JSON allocator to prevent heap fragmentation */
    cJSON_Hooks hooks;
    hooks.malloc_fn = json_malloc;
    hooks.free_fn = json_free;
    cJSON_InitHooks(&hooks);

    /* Debug UART — must be first for logging */
    Debug_Init();
    Boot_ReportResetReason();
    LOG_INFO(TAG_BOOT, "========================================");
    LOG_INFO(TAG_BOOT, "  IoT Visual Monitoring Firmware v%s  ", FW_VERSION);
    LOG_INFO(TAG_BOOT, "  Board: B-U585I-IOT02A               ");
    LOG_INFO(TAG_BOOT, "========================================");

    /* Status LED */
    BSP_LED_Init(LED_GREEN);
    BSP_LED_Init(LED_RED);
    BSP_LED_On(LED_GREEN);
    BSP_LED_On(LED_RED);

    /* RTC for scheduled wake-ups */
    RTC_Init();
    LOG_INFO(TAG_BOOT, "RTC initialized");

    /* ── OTA boot validation — must be early, after RTC init ── */
    OTA_ValidateBoot();

    /* B3 USER button — instant capture trigger */
    BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);
    LOG_INFO(TAG_BOOT, "B3 USER button initialized (short=capture, hold 3s=portal)");

    /* Stack canary painting — must happen early before deep calls */
#if STACK_WATERMARK_ENABLED
    RAM_PaintStackCanary();
#endif

    /* SEC-07: Initialize IWDG — must be after all blocking HAL inits */
#if WATCHDOG_ENABLED
    /* Ensure the IWDG is frozen during STOP2 BEFORE starting it. One-time
     * option-byte program (resets once on first boot after flashing). Without
     * this, any low-power sleep > ~16s would be reset by the watchdog. */
    Watchdog_FreezeInStop();
    Watchdog_Init();
#endif

    /* ── Phase 2: Connectivity ──────────────────────── */

    /* Wi-Fi — Enterprise credential chain:
     *   1. Try flash-stored credentials (with retries)
     *   2. If all fail → start captive portal (blocks until user configures) */
    {
        uint8_t wifi_connected = 0;

        /* ── Step 1: Try flash-stored credentials ── */
        WiFiCredentials_t flash_creds;
        if (WiFiCred_Load(&flash_creds) == WIFI_CRED_OK)
        {
            LOG_INFO(TAG_BOOT, "Found stored WiFi credentials: SSID='%s'", flash_creds.ssid);

            int wifi_retries = 0;
            while (wifi_retries < WIFI_CONNECT_RETRIES)
            {
                /* Re-init WiFi module if first attempt failed */
                if (wifi_retries > 0)
                {
                    WiFi_DeInit();
                    HAL_Delay(1000);
                    if (WiFi_Init() != WIFI_OK)
                    {
                        wifi_retries++;
                        continue;
                    }
                }
                else if (WiFi_Init() != WIFI_OK)
                {
                    wifi_retries++;
                    continue;
                }

                if (WiFi_Connect(flash_creds.ssid, flash_creds.password) == WIFI_OK)
                {
                    wifi_connected = 1;
                    LOG_INFO(TAG_BOOT, "Connected using stored credentials");

                    /* Populate runtime creds for automatic reconnects (e.g. sleep wake-up) */
                    strncpy(s_runtime_ssid, flash_creds.ssid, sizeof(s_runtime_ssid) - 1);
                    s_runtime_ssid[sizeof(s_runtime_ssid) - 1] = '\0';
                    strncpy(s_runtime_password, flash_creds.password, sizeof(s_runtime_password) - 1);
                    s_runtime_password[sizeof(s_runtime_password) - 1] = '\0';
                    s_has_runtime_wifi = 1;
                    break;
                }

                LOG_WARN(TAG_BOOT, "Wi-Fi connect failed, retrying... (%d/%d)",
                         wifi_retries + 1, WIFI_CONNECT_RETRIES);
                HAL_Delay(2000);
                wifi_retries++;
#if WATCHDOG_ENABLED
                HAL_IWDG_Refresh(&hiwdg);
#endif
            }
        }
        else
        {
            LOG_INFO(TAG_BOOT, "No stored WiFi credentials found in flash.");
        }

        /* ── Step 2: All failed → start captive portal ── */
        if (!wifi_connected)
        {
            LOG_WARN(TAG_BOOT, "All WiFi connection attempts failed — starting captive portal");
            CaptivePortal_Start();  /* Blocks until user configures WiFi + auto-reboots */
            /* CaptivePortal_Start calls NVIC_SystemReset — we never reach here */
        }
    }

    /* ── Time synchronization — set RTC from server clock ── */
    {
        uint8_t srv_h, srv_m, srv_s, srv_y, srv_mo, srv_d, srv_wd;
        if (WiFi_HttpGetTime(&srv_h, &srv_m, &srv_s,
                             &srv_y, &srv_mo, &srv_d, &srv_wd) == WIFI_OK)
        {
            /* Convert BIN → BCD for RTC registers */
            #define BIN2BCD(x) (((x) / 10) << 4 | ((x) % 10))
            uint32_t tr = ((uint32_t)BIN2BCD(srv_h) << 16)
                        | ((uint32_t)BIN2BCD(srv_m) << 8)
                        | ((uint32_t)BIN2BCD(srv_s));

            uint32_t dr = ((uint32_t)BIN2BCD(srv_y) << 16)
                        | ((uint32_t)srv_wd << 13)
                        | ((uint32_t)BIN2BCD(srv_mo) << 8)
                        | ((uint32_t)BIN2BCD(srv_d));

            /* Unlock write protection */
            RTC->WPR = 0xCA;
            RTC->WPR = 0x53;

            /* Enter init mode */
            RTC->ICSR |= RTC_ICSR_INIT;
            uint32_t tick = HAL_GetTick();
            while (!(RTC->ICSR & RTC_ICSR_INITF))
            {
                if ((HAL_GetTick() - tick) > 2000) break;
            }

            /* Write time and date */
            RTC->TR = tr;
            RTC->DR = dr;

            /* Exit init mode */
            RTC->ICSR &= ~RTC_ICSR_INIT;

            /* Re-lock write protection */
            RTC->WPR = 0xFF;

            LOG_INFO(TAG_BOOT, "RTC synced to server: %02u:%02u:%02u  20%02u-%02u-%02u",
                     srv_h, srv_m, srv_s, srv_y, srv_mo, srv_d);

            /* Verify RTC is actually ticking after 1 second */
            HAL_Delay(1000);

            /* Direct read (read TR then DR — must read both per RM) */
            uint32_t tr_read = RTC->TR;
            uint32_t dr_read = RTC->DR;
            (void)dr_read;

            #define BCD2BIN(x) (((x) >> 4) * 10 + ((x) & 0x0F))
            uint8_t rh = BCD2BIN((tr_read >> 16) & 0x3F);
            uint8_t rm = BCD2BIN((tr_read >> 8) & 0x7F);
            uint8_t rs = BCD2BIN(tr_read & 0x7F);
            
            (void)rh; (void)rm; (void)rs;

            LOG_INFO(TAG_BOOT, "RTC verify (1s later): %02u:%02u:%02u  ICSR=0x%08lX  BDCR=0x%08lX",
                     rh, rm, rs,
                     (unsigned long)RTC->ICSR,
                     (unsigned long)RCC->BDCR);
        }
        else
        {
            LOG_WARN(TAG_BOOT, "Time sync failed — RTC uses default 00:00:00");
        }
    }

    /* MQTT */
    MQTTConfig_t mqtt_cfg = {
        .broker_host = MQTT_BROKER_HOST,
        .broker_port = MQTT_BROKER_PORT,
        .client_id   = MQTT_CLIENT_ID,
    };

    /* Flush SPI pipeline — drain any pending responses before MQTT */
    MX_WIFI_IO_YIELD(wifi_obj_get(), 2000);

    if (MQTT_Init(&mqtt_cfg) != 0)
    {
        LOG_ERROR(TAG_BOOT, "FATAL: MQTT connect failed");
        LED_SignalError();
    }

    if (MQTT_SubscribeCommands(on_command_received) != 0)
    {
        LOG_ERROR(TAG_BOOT, "FATAL: MQTT subscribe failed");
        LED_SignalError();
    }

    /* ── Phase 3: Publish online status ─────────────── */

    MQTT_PublishStatus("{\"status\":\"online\",\"firmware\":\"" FW_VERSION "\"}");
    LOG_INFO(TAG_BOOT, "Boot complete (v%s) — waiting for commands...", FW_VERSION);

    /* All subsystems initialized OK — mark boot as successful.
     * This resets the OTA boot failure counter, preventing rollback. */
    OTA_MarkBootSuccessful();

    /* ── Hyper-distinct Board Ready Pattern ── */
    /* 3 crisp GREEN flashes indicate the board is fully initialized and online */
    for (int i = 0; i < 3; i++) {
        BSP_LED_On(LED_GREEN);
        HAL_Delay(100);
        BSP_LED_Off(LED_GREEN);
        HAL_Delay(100);
    }
    
    /* Turn OFF RED LED since boot is now fully complete (it was turned ON at init) */
    BSP_LED_Off(LED_RED);

    /* ── Phase 3b: Persistent Camera Init ──────────────
     *
     * Enterprise optimization: initialize the camera ONCE at boot.
     * The sensor stays powered and AEC-converged, enabling sub-second
     * warm captures for the entire uptime of the board. */
#if WATCHDOG_ENABLED
    HAL_IWDG_Refresh(&hiwdg);
#endif
    if (Camera_Init(CAMERA_DEFAULT_RESOLUTION) != CAMERA_OK)
    {
        LOG_ERROR(TAG_BOOT, "WARNING: Camera init failed at boot — captures will cold-start");
    }
    else
    {
        LOG_INFO(TAG_BOOT, "Camera warm and ready (persistent mode)");
    }

    /* ── Phase 4: Unified Main Loop ───────────────────
     *
     * Runs forever. Handles all commands inline:
     *  - capture_now, capture_sequence, button captures
     *  - Scheduled tasks: polls RTC and executes when time arrives
     *  - sleep_mode: only enters STOP2 when explicitly enabled
     *
     * The board stays awake and connected to MQTT by default.
     */

    uint32_t poll_start    = HAL_GetTick();
    (void)poll_start;
    uint32_t last_ping     = HAL_GetTick();
    uint32_t last_ota_check = HAL_GetTick();

    /* Anchor the energy wall-clock window to boot RTC time, so ps_rest_ms
     * (computed at publish as wall_ms - awake_tick_ms) is valid from window 1.
     * RTC runs continuously through STOP2; HAL_GetTick does not. */
    {
        RTC_TimeTypeDef _ei_t;
        RTC_DateTypeDef _ei_d;
        HAL_RTC_GetTime(&hrtc, &_ei_t, RTC_FORMAT_BIN);
        HAL_RTC_GetDate(&hrtc, &_ei_d, RTC_FORMAT_BIN);  /* RM: read date after time */
        s_energy_wall_start = (uint32_t)_ei_t.Hours * 3600u
                            + (uint32_t)_ei_t.Minutes * 60u
                            + (uint32_t)_ei_t.Seconds;
    }

    /* Restore persisted LP mode: auto-resume PS-REST after any reboot.
     * Safe: EnterPSRest() guards on MQTT_IsConnected() -- restoring
     * lp_mode=1 with MQTT connected simply starts PS-REST immediately. */
    {
        uint8_t restored_lp = 0u;
        if (BoardSettings_LoadLPMode(&restored_lp) == BSET_OK && restored_lp <= 1u)
        {
            s_lp_mode = restored_lp;
            if (s_lp_mode == 1)
                LOG_INFO(TAG_BOOT, "PS-REST restored from flash settings");
        }
    }

    while (1)
    {
        MQTT_ProcessLoop();

        /* SEC-07: Refresh watchdog every loop iteration */
#if WATCHDOG_ENABLED
        HAL_IWDG_Refresh(&hiwdg);
#endif

        /* Send MQTT keepalive ping and online status every STATUS_HEARTBEAT_INTERVAL_MS (instant-feel dashboard) */
        if ((HAL_GetTick() - last_ping) > STATUS_HEARTBEAT_INTERVAL_MS)
        {
            if (MQTT_IsConnected()) 
            {
                MQTT_SendPing();
                
                /* OBS-01, OBS-02: Enhanced heartbeat telemetry */
                char heartbeat[256];
                snprintf(heartbeat, sizeof(heartbeat),
                    "{\"status\":\"online\",\"firmware\":\"" FW_VERSION "\","
                    "\"uptime_s\":%lu,\"wifi_reconnects\":%lu,"
                    "\"mqtt_reconnects\":%lu,\"capture_failures\":%lu,"
                    "\"ota_failures\":%lu,\"lp_mode\":\"%s\",\"sleep_mode\":%d}",
                    (unsigned long)(HAL_GetTick() / 1000),
                    (unsigned long)s_telemetry.wifi_reconnects,
                    (unsigned long)s_telemetry.mqtt_reconnects,
                    (unsigned long)s_telemetry.capture_failures,
                    (unsigned long)s_telemetry.ota_failures,
                    s_lp_mode ? "ps_rest" : "normal",
                    s_sleep_enabled ? 1 : 0);
                    
                static uint8_t s_publish_failures = 0;
                if (MQTT_PublishStatus(heartbeat) != 0) {
                    s_publish_failures++;
                    if (s_publish_failures >= MQTT_MAX_PUBLISH_FAILURES) {
                        LOG_ERROR(TAG_MQTT, "MQTT connection lost (publish failures)");
                        s_telemetry.mqtt_reconnects++;
                        MQTT_Disconnect();
                        MQTT_Init(&mqtt_cfg);
                        MQTT_SubscribeCommands(on_command_received);
                        s_publish_failures = 0;
                    }
                } else {
                    s_publish_failures = 0;
                }
            } 
            else 
            {
                LOG_WARN(TAG_MQTT, "MQTT offline, attempting reconnect...");
                s_telemetry.mqtt_reconnects++;
                /* WiFi L2 reconnect: the EMW3080 in powersave mode does not
                 * auto-reassociate after a prolonged AP outage. Without this,
                 * MQTT_Init() fails immediately every 5 s and the board spins
                 * offline indefinitely (IWDG refreshed by the fast poll loop
                 * so no watchdog reset -- silent indefinite failure). */
                if (s_has_runtime_wifi && MX_WIFI_IsConnected(wifi_obj_get()) <= 0)
                {
                    LOG_WARN(TAG_WIFI, "WiFi L2 lost -- reconnecting before MQTT");
                    s_telemetry.wifi_reconnects++;
                    Watchdog_Refresh();
                    WiFi_DeInit();
                    WiFi_Init();
                    WiFi_Connect(s_runtime_ssid, s_runtime_password);
                    Watchdog_Refresh();
                }
                MQTT_Init(&mqtt_cfg);
                MQTT_SubscribeCommands(on_command_received);
            }
            last_ping = HAL_GetTick();
        }

        /* Energy phase-timer: publish state-time breakdown roughly once per
         * minute (WALL-CLOCK) when PS-REST is active, enabling server-side
         * duty-cycle energy modelling.  SysTick (HAL_GetTick) freezes during
         * STOP2 and is not corrected on wake, so the window MUST be timed by the
         * RTC (runs through STOP2).  ps_rest_ms = wall_ms - awake_tick_ms. */
        if (s_lp_mode == 1 && MQTT_IsConnected())
        {
            RTC_TimeTypeDef _rtc_now;
            RTC_DateTypeDef _rtc_now_d;
            HAL_RTC_GetTime(&hrtc, &_rtc_now, RTC_FORMAT_BIN);
            HAL_RTC_GetDate(&hrtc, &_rtc_now_d, RTC_FORMAT_BIN);  /* RM: date after time */
            uint32_t _now_s  = (uint32_t)_rtc_now.Hours * 3600u
                             + (uint32_t)_rtc_now.Minutes * 60u
                             + (uint32_t)_rtc_now.Seconds;
            uint32_t _wall_s = (_now_s >= s_energy_wall_start)
                             ? (_now_s - s_energy_wall_start)
                             : (_now_s + 86400u - s_energy_wall_start);

            if ((_wall_s * 1000u) >= ENERGY_REPORT_INTERVAL_MS)
            {
                uint32_t wall_ms    = _wall_s * 1000u;
                uint32_t tick_ms    = HAL_GetTick() - s_energy_report_ms; /* awake */
                uint32_t ps_rest_ms = (wall_ms > tick_ms) ? (wall_ms - tick_ms) : 0u;

                char emsg[160];
                snprintf(emsg, sizeof(emsg),
                         "{\"status\":\"energy\",\"window_ms\":%lu,"
                         "\"ps_rest_ms\":%lu,\"capture_ms\":%lu}",
                         (unsigned long)wall_ms,
                         (unsigned long)ps_rest_ms,
                         (unsigned long)s_energy_capture_ms);
                MQTT_PublishStatus(emsg);

                s_energy_capture_ms = 0;
                s_energy_report_ms  = HAL_GetTick();
                s_energy_wall_start = _now_s;
            }
        }

        /* Idle heartbeat: IDLE_BLINK_ON_MS GREEN blip every IDLE_BLINK_PERIOD_MS to prove we are alive */
        static uint32_t last_idle_blink = 0;
        static uint8_t idle_led_on = 0;
        
        if (!idle_led_on && (HAL_GetTick() - last_idle_blink) >= IDLE_BLINK_PERIOD_MS)
        {
            BSP_LED_On(LED_GREEN);
            idle_led_on = 1;
            last_idle_blink = HAL_GetTick();
        }
        else if (idle_led_on && (HAL_GetTick() - last_idle_blink) >= IDLE_BLINK_ON_MS)
        {
            BSP_LED_Off(LED_GREEN);
            idle_led_on = 0;
            /* Keep last_idle_blink anchor for stable IDLE_BLINK_PERIOD_MS period */
        }
        /* ── PS-REST gate ──
         * When the agent has enabled PS-REST (low_power_mode=ps_rest) and no
         * work is pending, enter WIFI_PS_REST: WiFi stays associated in 802.11
         * power-save and the MCU sleeps in STOP2, waking on the REST_POLL_S RTC
         * alarm floor or sub-second on the NOTIFY/PD14 line when a command
         * arrives. Verified on-device 2026-05-28 (wake-on-ping 0.1-2.6s, MQTT
         * survives, capture pipeline runs from rest). This was only possible
         * after fixing two STOP2-wake bugs that had never been exercised
         * (LOW_POWER_MODE_ENABLED was 0): the IWDG_STOP option-byte mask
         * (Watchdog_FreezeInStop) and the STM32U5 Smart-Run-Domain RTC clock
         * (SRDAMR.RTCAPBAMEN, HAL_RTC_MspInit). Default behavior is unchanged:
         * s_lp_mode defaults to 0 so this path is never taken unless the agent
         * opts in. The 7 work-flags below ensure we never sleep with work queued;
         * IWDG is refreshed on every wake inside EnterPSRest(). */
        if (s_lp_mode == 1
            && _capture_queue_empty()
            && !s_sequence_requested
            && !s_button_held
            && !s_capture_now_requested
            && !s_wifi_reconfig_requested
            && !s_ota_requested
            && !s_portal_requested
            && !s_ping_requested)
        {
            EnterPSRest();
        }
        else
        {
            HAL_Delay(SCHEDULER_MQTT_POLL_MS);
        }

        /* ── Handle: Capture Queue (MQTT commands) ── */
        /* Dequeue one capture per loop iteration. Each capture+upload takes ~7s,
         * so queued requests execute sequentially without dropping any. */
        if (!_capture_queue_empty())
        {
            uint32_t queued_task_id = s_capture_queue[s_capture_queue_tail];
            s_capture_queue_tail = (s_capture_queue_tail + 1) % CAPTURE_QUEUE_SIZE;

            s_capture_now_task_id = queued_task_id;
            LOG_INFO(TAG_BOOT, "Dequeuing capture task_id=%lu (%d remaining)",
                     (unsigned long)queued_task_id, _capture_queue_count());
            {
                uint32_t _t_cap = HAL_GetTick();
                _do_capture_now();
                s_energy_capture_ms += HAL_GetTick() - _t_cap;
            }
            poll_start = HAL_GetTick();
        }

        /* ── Handle: Capture Sequence (MQTT command) ── */
        if (s_sequence_requested)
        {
            s_sequence_requested = 0;
            _do_capture_sequence();
            poll_start = HAL_GetTick();
        }

        /* ── Handle: B3 button (short = capture, long ≥3s = portal) ── */
        if (s_button_held)
        {
            uint32_t held_ms = HAL_GetTick() - s_button_press_tick;
            int still_pressed = (BSP_PB_GetState(BUTTON_USER) != 0);

            /* LED feedback: RED on after 1s to signal "keep holding for portal" */
            if (held_ms >= 1000 && still_pressed && !s_button_led_on)
            {
                BSP_LED_On(LED_RED);
                s_button_led_on = 1;
            }

            if (held_ms >= BUTTON_LONG_PRESS_MS && still_pressed)
            {
                /* ── Long press confirmed → enter portal mode ── */
                s_button_held = 0;
                s_button_led_on = 0;
                BSP_LED_Off(LED_RED);

                /* Confirmation flash: 5× rapid green blinks */
                for (int i = 0; i < 5; i++) {
                    BSP_LED_On(LED_GREEN);
                    HAL_Delay(100);
                    BSP_LED_Off(LED_GREEN);
                    HAL_Delay(100);
                }

                LOG_INFO(TAG_BOOT, "=== LONG PRESS — entering WiFi setup portal ===");
                MQTT_PublishStatus("{\"status\":\"portal_button\"}");
                s_portal_requested = 1;
            }
            else if (!still_pressed)
            {
                /* ── Button released → short press = capture ── */
                s_button_held = 0;
                if (s_button_led_on) {
                    BSP_LED_Off(LED_RED);
                    s_button_led_on = 0;
                }

                if (held_ms >= BUTTON_DEBOUNCE_MS)
                {
                    _do_button_capture();
                }
                poll_start = HAL_GetTick();
            }
        }

        /* ── Handle: OTA firmware update (MQTT command) ── */
        if (s_ota_requested)
        {
            s_ota_requested = 0;
#if WATCHDOG_ENABLED
            HAL_IWDG_Refresh(&hiwdg);  /* OTA is long — refresh before starting */
#endif
            _do_ota_update();
            poll_start = HAL_GetTick();
        }

        /* ── Handle: WiFi credential reconfiguration (MQTT command) ── */
        if (s_wifi_reconfig_requested)
        {
            s_wifi_reconfig_requested = 0;
            _do_wifi_reconfig();
            poll_start = HAL_GetTick();
        }

        /* ── Handle: Ping command ── */
        if (s_ping_requested)
        {
            s_ping_requested = 0;
            _do_ping_sequence();
            poll_start = HAL_GetTick();
        }

        /* ── Handle: Captive Portal (MQTT command) ── */
        if (s_portal_requested)
        {
            s_portal_requested = 0;
            _do_start_portal();
            poll_start = HAL_GetTick();
        }

        /* ── Enterprise OTA Architecture: Hybrid Push/Pull ──
         * Server PUSHES {"type":"firmware_update"} via MQTT for instant OTAs.
         * Firmware also runs a background daemon pulling versions every OTA_CHECK_INTERVAL_MS
         * to guarantee no node is ever stranded on deprecated firmware. ── */
        if ((HAL_GetTick() - last_ota_check) > OTA_CHECK_INTERVAL_MS)
        {
            if (!s_ota_requested && !s_ota_in_progress && MQTT_IsConnected())
            {
                OTAVersionInfo_t check_info;
                if (OTA_CheckForUpdate(&check_info) == OTA_OK)
                {
                    LOG_INFO(TAG_OTA, "Autonomous daemon detected new firmware v%s! Triggering...", check_info.version);
                    s_ota_requested = 1; /* Delegate full download to the OTA block on next loop */
                }
            }
            last_ota_check = HAL_GetTick();
        }

        /* ── Handle: Scheduled Tasks (RTC polling) ── */
        if (s_schedule_received && s_schedule.current_index < s_schedule.task_count)
        {
            const ScheduledTask_t *next = &s_schedule.tasks[s_schedule.current_index];

            /* Read current RTC time */
            RTC_TimeTypeDef now_time;
            RTC_DateTypeDef now_date;
            HAL_RTC_GetTime(&hrtc, &now_time, RTC_FORMAT_BIN);
            HAL_RTC_GetDate(&hrtc, &now_date, RTC_FORMAT_BIN);

            int32_t now_secs  = (int32_t)now_time.Hours * 3600
                              + (int32_t)now_time.Minutes * 60
                              + (int32_t)now_time.Seconds;
            int32_t task_secs = (int32_t)next->hour * 3600
                              + (int32_t)next->minute * 60
                              + (int32_t)next->second;

            int32_t diff = task_secs - now_secs;
            if (diff < 0) {
                diff += 86400;
            }

            /* Within 30 seconds of the target time (handles late schedule arrival) */
            if (diff <= 30 || diff >= 86370)
            {
                /* ── Time reached — execute this task ── */
                LOG_INFO(TAG_SCHED, "Executing task %u: %02u:%02u:%02u '%s'",
                         next->task_id, next->hour, next->minute, next->second,
                         next->objective);

                poll_start = HAL_GetTick();

                char status_msg[128];
                snprintf(status_msg, sizeof(status_msg),
                         "{\"status\":\"executing\",\"task_id\":%u,\"action\":\"CAPTURE_IMAGE\"}",
                         next->task_id);
                MQTT_PublishStatus(status_msg);

                if (next->action == ACTION_CAPTURE_IMAGE)
                {
                    LOG_INFO(TAG_BOOT, "=== Task %u: CAPTURE_IMAGE ===", next->task_id);

                    uint32_t captured_size = 0;
                    CameraStatus_t cam_ret;

                    /* Turn on RED LED to indicate Image Capturing state */
                    BSP_LED_On(LED_RED);

                    /* Use warm capture if camera is already initialized */
                    if (Camera_IsInitialized())
                    {
                        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"capturing\",\"task_id\":%u}", next->task_id);
                        MQTT_PublishStatus(status_msg);

                        cam_ret = Camera_WarmCapture(
                            s_image_buffer, sizeof(s_image_buffer), &captured_size);
                    }
                    else
                    {
                        LOG_WARN(TAG_CAM, "Camera cold — initializing for task %u", next->task_id);
                        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"camera_init\",\"task_id\":%u}", next->task_id);
                        MQTT_PublishStatus(status_msg);

                        if (Camera_Init(CAMERA_DEFAULT_RESOLUTION) != CAMERA_OK)
                        {
                            LOG_ERROR(TAG_CAM, "Camera init failed for task %u", next->task_id);
                            cam_ret = CAMERA_ERROR_INIT;
                        }
                        else
                        {
                            snprintf(status_msg, sizeof(status_msg), "{\"status\":\"capturing\",\"task_id\":%u}", next->task_id);
                            MQTT_PublishStatus(status_msg);

                            cam_ret = Camera_CaptureFrame(
                                s_image_buffer, sizeof(s_image_buffer), &captured_size);
                        }
                    }

                    if (cam_ret == CAMERA_OK && captured_size > 0)
                    {
                        snprintf(status_msg, sizeof(status_msg),
                                 "{\"status\":\"uploading\",\"task_id\":%u}",
                                 next->task_id);
                        MQTT_PublishStatus(status_msg);

                        WiFiStatus_t upload_ret = WiFi_HttpPostImage(
                            SERVER_UPLOAD_URL, next->task_id,
                            s_image_buffer, captured_size);

                        if (upload_ret == WIFI_OK)
                        {
                            LOG_INFO(TAG_HTTP, "Task %u upload OK (%lu bytes)",
                                     next->task_id, (unsigned long)captured_size);
                            snprintf(status_msg, sizeof(status_msg),
                                     "{\"status\":\"uploaded\",\"task_id\":%u,\"bytes\":%lu}",
                                     next->task_id, (unsigned long)captured_size);
                        }
                        else
                        {
                            LOG_ERROR(TAG_HTTP, "Task %u upload failed", next->task_id);
                            snprintf(status_msg, sizeof(status_msg),
                                     "{\"status\":\"error\",\"task_id\":%u,\"reason\":\"upload_failed\"}",
                                     next->task_id);
                        }

                        /* Turn off RED LED when upload completes */
                        BSP_LED_Off(LED_RED);
                        
                        /* SEC-10: Cryptographic Sanitization of Image Buffer */
                        secure_erase(s_image_buffer, captured_size);

                        /* Re-establish MQTT only if connection was lost during upload */
                        if (!MQTT_IsConnected())
                        {
                            if (MQTT_Init(&mqtt_cfg) == 0)
                            {
                                MQTT_SubscribeCommands(on_command_received);
                            }
                        }
                        MQTT_PublishStatus(status_msg);
                    }
                    else
                    {
                        LOG_ERROR(TAG_CAM, "Capture failed for task %u", next->task_id);
                        BSP_LED_Off(LED_RED);
                    }

#if STACK_WATERMARK_ENABLED
                    RAM_CheckStackHighWater();
#endif
                }

                /* Advance to next task */
                s_schedule.current_index++;

                if (s_schedule.current_index >= s_schedule.task_count)
                {
                    LOG_INFO(TAG_SCHED, "All %d tasks completed for today! Resetting cycle parameters for tomorrow.", s_schedule.task_count);
                    s_schedule.current_index = 0;
                    MQTT_PublishStatus("{\"status\":\"cycle_complete\"}");
                }
            }
            else if (s_sleep_enabled)
            {
                /* ── Sleep mode: enter STOP2 until next task time ── */
                LOG_INFO(TAG_PWR, "Sleep mode — STOP2 until %02u:%02u:%02u",
                         next->hour, next->minute, next->second);

                int alarm_result = Scheduler_SetNextAlarm(&s_schedule);
                if (alarm_result == 0)
                {
                    /* Announce DEEP_DORMANT so the server-side command queue
                     * holds any commands until we wake + reconnect (WiFi is
                     * fully off during dormancy — the board is deaf until the
                     * scheduled wake or a B3 press). */
                    {
                        char dormant_msg[80];
                        snprintf(dormant_msg, sizeof(dormant_msg),
                                 "{\"status\":\"deep_dormant\",\"until\":\"%02u:%02u:%02u\"}",
                                 next->hour, next->minute, next->second);
                        MQTT_PublishStatus(dormant_msg);
                        HAL_Delay(50);  /* let the publish flush before disconnect */
                    }
                    MQTT_Disconnect();
                    WiFi_DeInit();
                    Camera_DeInit();  /* Power down camera for sleep */
                    uint32_t btn_tick_before = s_button_press_tick;
                    Scheduler_EnterLowPower();

                    /* Refresh IWDG immediately on wake: the reconnect sequence
                     * below (WiFi assoc + retries) can take several seconds, and
                     * the IWDG resumes counting the moment we leave STOP2. Reset
                     * its counter now so the wake reinit can't trip it. */
#if WATCHDOG_ENABLED
                    HAL_IWDG_Refresh(&hiwdg);
#endif

                    /* ── ANTI-BRICK: B3 button force-wake override ──
                     * The B3 button is an EXTI source and wakes STOP2. If the
                     * button (not the RTC alarm) woke us, the operator is
                     * physically asking the board to stay awake — clear the
                     * sleep opt-in so the board returns to ACTIVE and is fully
                     * agent-reachable. This is the always-works physical escape
                     * hatch that guarantees the board can never be bricked into
                     * unreachable sleep. */
                    if (s_button_press_tick != btn_tick_before)
                    {
                        s_sleep_enabled = 0;
                        s_button_held = 0;
                        LOG_WARN(TAG_PWR, "B3 woke board from STOP2 — sleep DISABLED, staying ACTIVE");
                    }

                    /* Woke up — reconnect everything using runtime creds */
                    WiFi_Init();
                    WiFi_Connect(s_runtime_ssid, s_runtime_password);
                    MQTT_Init(&mqtt_cfg);
                    MQTT_SubscribeCommands(on_command_received);
                    last_ping = HAL_GetTick();
                    if (s_sleep_enabled == 0)
                    {
                        MQTT_PublishStatus("{\"status\":\"awake\",\"reason\":\"button_override\"}");
                    }

                    /* Re-initialize camera after sleep wake-up */
                    Camera_Init(CAMERA_DEFAULT_RESOLUTION);
                }
            }
            /* else: awake mode — just keep polling next iteration */
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  System Clock Configuration (160 MHz via PLL from MSI)
 * ═══════════════════════════════════════════════════════════════════════════ */

void SystemClock_Config(void)
{
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);
    HAL_PWREx_ConfigSupply(PWR_SMPS_SUPPLY);
    __HAL_RCC_PWR_CLK_DISABLE();

    /* MSI 4MHz → PLL × 80 / 2 = 160 MHz */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_MSI | RCC_OSCILLATORTYPE_LSE;
    RCC_OscInitStruct.MSIState = RCC_MSI_ON;
    RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_4;
    RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.LSEState = RCC_LSE_ON;  /* LSE needed for RTC */
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLMBOOST = RCC_PLLMBOOST_DIV1;
    RCC_OscInitStruct.PLL.PLLM = 1;
    RCC_OscInitStruct.PLL.PLLN = 80;
    RCC_OscInitStruct.PLL.PLLR = 2;
    RCC_OscInitStruct.PLL.PLLP = 2;
    RCC_OscInitStruct.PLL.PLLQ = 2;
    RCC_OscInitStruct.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        while (1);
    }

    RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                                   RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                                   RCC_CLOCKTYPE_PCLK3);
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
        while (1);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTC MSP Init — empty stub to prevent weak default from interfering.
 *  All RTC clock setup is done directly in RTC_Init via registers.
 * ═══════════════════════════════════════════════════════════════════════════ */

void HAL_RTC_MspInit(RTC_HandleTypeDef *hrtc_unused)
{
    (void)hrtc_unused;
    /* Intentionally empty — RTC_Init handles everything directly */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTC Initialization (LSI clock source)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void RTC_Init(void)
{
    /* ═══ Direct register RTC init — HAL_RTC_Init was failing ═══ */

    /* 1. Enable PWR clock + backup domain access */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* 2. Reset backup domain to clear any stale RTC state */
    __HAL_RCC_BACKUPRESET_FORCE();
    for (volatile int i = 0; i < 100; i++);  /* brief delay */
    __HAL_RCC_BACKUPRESET_RELEASE();
    for (volatile int i = 0; i < 100; i++);

    /* 3. Enable LSI and wait for ready (STM32U5: LSI bits are in BDCR) */
    RCC->BDCR |= RCC_BDCR_LSION;
    uint32_t tick = HAL_GetTick();
    while (!(RCC->BDCR & RCC_BDCR_LSIRDY))
    {
        if ((HAL_GetTick() - tick) > 2000)
        {
            LOG_ERROR(TAG_BOOT, "LSI timeout");
            break;
        }
    }
    LOG_DEBUG(TAG_BOOT, "LSI ready: %s, BDCR=0x%08lX",
              (RCC->BDCR & RCC_BDCR_LSIRDY) ? "YES" : "NO",
              (unsigned long)RCC->BDCR);

    /* 4. Select LSI as RTC clock source and enable RTC.
     * CRITICAL: must preserve LSION bit! RTCSEL field = bits 9:8 only. */
    uint32_t bdcr = RCC->BDCR;
    bdcr &= ~(0x3UL << 8);     /* Clear only RTCSEL bits [9:8] */
    bdcr |= (0x2UL << 8);      /* RTCSEL = 10 = LSI */
    bdcr |= RCC_BDCR_RTCEN;    /* Enable RTC */
    RCC->BDCR = bdcr;

    /* Enable RTC APB bus clock — CRITICAL on STM32U5!
     * Without this, CPU reads from RTC registers return 0. */
    __HAL_RCC_RTCAPB_CLK_ENABLE();

    /* Enable RTC clock in the Smart-Run-Domain AUTONOMOUS + Sleep/Stop registers.
     * CRITICAL for STOP2 wake on STM32U5 (RM0456 §11.8.44/45): without
     * SRDAMR.RTCAPBAMEN ("RTC Autonomous Mode Enable in Stop 0,1,2") the RTC
     * alarm flag still sets but the CPU does NOT exit Stop — verified on-device:
     * the board slept through its 3s alarm and only the (then-unfrozen) IWDG
     * recovered it. This is the documented, low-visibility U5 gotcha; no EXTI
     * line config is needed (that is legacy L4/F4). */
    __HAL_RCC_RTCAPB_CLKAM_ENABLE();     /* SRDAMR.RTCAPBAMEN — wake from STOP0/1/2 */
    __HAL_RCC_RTCAPB_CLK_SLEEP_ENABLE(); /* APB3SMENR.RTCAPBSMEN — clocked in sleep/stop */

    LOG_DEBUG(TAG_BOOT, "After RTCSEL: BDCR=0x%08lX (LSION=%lu LSIRDY=%lu)",
              (unsigned long)RCC->BDCR,
              (unsigned long)(RCC->BDCR & RCC_BDCR_LSION),
              (unsigned long)(RCC->BDCR & RCC_BDCR_LSIRDY));

    /* 5. Prepare the HAL handle (for HAL_RTC_SetTime/GetTime/Alarm later) */
    hrtc.Instance = RTC;
    hrtc.State = HAL_RTC_STATE_READY;

    /* 6. Disable write protection (magic keys) */
    RTC->WPR = 0xCA;
    RTC->WPR = 0x53;

    /* 7. Enter initialization mode */
    RTC->ICSR |= RTC_ICSR_INIT;
    tick = HAL_GetTick();
    while (!(RTC->ICSR & RTC_ICSR_INITF))
    {
        if ((HAL_GetTick() - tick) > 2000)
        {
            LOG_ERROR(TAG_BOOT, "RTC INITF timeout");
            break;
        }
    }

    LOG_DEBUG(TAG_BOOT, "RTC INITF: %s, BDCR=0x%08lX",
              (RTC->ICSR & RTC_ICSR_INITF) ? "OK" : "FAIL",
              (unsigned long)RCC->BDCR);

    /* 8. Set prescalers for LSI (~32 kHz): PREDIV_A=127, PREDIV_S=249 → ~1Hz */
    RTC->PRER = (127 << 16) | 249;

    /* 9. Set 24h format, time = 00:00:00 */
    RTC->CR &= ~RTC_CR_FMT;  /* 24h */
    RTC->TR = 0x00000000;     /* 00:00:00 in BCD */
    RTC->DR = 0x00012101;     /* Mon, 01-Jan-2026: YT=2,YU=6,MT=0,MU=1,DT=0,DU=1,WDU=1 */

    /* 10. Exit initialization mode */
    RTC->ICSR &= ~RTC_ICSR_INIT;

    /* Re-enable write protection */
    RTC->WPR = 0xFF;

    LOG_INFO(TAG_BOOT, "RTC initialized (direct register, LSI)");

    /* Enable RTC Alarm A interrupt */
    HAL_NVIC_SetPriority(RTC_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(RTC_IRQn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Cache Enable
 * ═══════════════════════════════════════════════════════════════════════════ */

static void CACHE_Enable(void)
{
    HAL_ICACHE_ConfigAssociativityMode(ICACHE_1WAY);
    HAL_ICACHE_Enable();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Error Signalling
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Assert a hardware reset on a fatal error, saving the state in TAMP.
 * Fintech best practice: bricking requires autonomous recovery.
 */
static void LED_SignalError(void)
{
    LOG_ERROR(TAG_BOOT, "FATAL ERROR — Asserting NVIC System Reset");
    
    /* Enable backup domain access if not already */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    
    /* Store fatal fault code (0xDEAD0001) in backup register */
    TAMP->BKP5R = 0xDEAD0001;
    
    /* Short delay for logs to flush if using UART, then hard reset */
    HAL_Delay(100);
    NVIC_SystemReset();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SEC-07: IWDG Initialization
 *
 *  LSI ≈ 32 kHz, Prescaler = /256 → 1 tick ≈ 8ms.
 *  Reload = WATCHDOG_TIMEOUT_S * 125 → timeout in seconds.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if WATCHDOG_ENABLED
/*
 * Freeze the IWDG during STOP2 via the IWDG_STOP user option byte.
 *
 * CRITICAL for low-power: on STM32U5 the IWDG is clocked by LSI, which keeps
 * running in STOP2. With the default option (IWDG_STOP=RUN) the watchdog would
 * keep counting while the MCU sleeps and reset the board ~16s into any sleep
 * longer than the timeout — bricking every realistic schedule. Programming the
 * option byte to FREEZE stops the IWDG counter during STOP2; it still protects
 * the active/execute phases (refreshed in the main loop).
 *
 * This is a one-time persistent option-byte write. It is GUARDED: if the bit is
 * already FREEZE we do nothing (no reset loop). HAL_FLASH_OB_Launch() applies
 * the new option bytes via a system reset, so on the first boot after flashing
 * the board resets once; every subsequent boot sees FREEZE and skips.
 */
static void Watchdog_FreezeInStop(void)
{
    FLASH_OBProgramInitTypeDef ob = {0};
    HAL_FLASHEx_OBGetConfig(&ob);

    /* USERConfig holds the raw FLASH->OPTR bits (FLASH_OB_GetUser), so the
     * IWDG_STOP state must be read with the OPTR BIT mask (FLASH_OPTR_IWDG_STOP,
     * bit 17 = 0x20000) — NOT OB_USER_IWDG_STOP (0x40), which is the HAL
     * *selector* used only by OBProgram. Masking the selector tested bit 6 and
     * ALWAYS reported "frozen" (false positive), so the freeze was never
     * programmed and the IWDG kept running in STOP — resetting the board ~16s
     * into every sleep (confirmed on-device 2026-05-28). OB_IWDG_STOP_FREEZE==0,
     * OB_IWDG_STOP_RUN==FLASH_OPTR_IWDG_STOP. */
    if ((ob.USERConfig & FLASH_OPTR_IWDG_STOP) != OB_IWDG_STOP_FREEZE)
    {
        LOG_WARN(TAG_BOOT, "IWDG set to RUN-in-STOP — programming FREEZE (one-time, will reset)");

        if (HAL_FLASH_Unlock() != HAL_OK || HAL_FLASH_OB_Unlock() != HAL_OK)
        {
            LOG_ERROR(TAG_BOOT, "Option-byte unlock failed — IWDG_STOP unchanged");
            return;
        }

        FLASH_OBProgramInitTypeDef prog = {0};
        prog.OptionType  = OPTIONBYTE_USER;
        prog.USERType    = OB_USER_IWDG_STOP;
        prog.USERConfig  = OB_IWDG_STOP_FREEZE;

        if (HAL_FLASHEx_OBProgram(&prog) != HAL_OK)
        {
            LOG_ERROR(TAG_BOOT, "IWDG_STOP option-byte program failed");
            HAL_FLASH_OB_Lock();
            HAL_FLASH_Lock();
            return;
        }

        /* Applies the new option bytes — triggers a system reset. */
        HAL_FLASH_OB_Launch();

        /* Not normally reached (reset above); lock defensively if it returns. */
        HAL_FLASH_OB_Lock();
        HAL_FLASH_Lock();
    }
    else
    {
        LOG_INFO(TAG_BOOT, "IWDG already frozen in STOP — safe for low-power sleep");
    }
}

void Watchdog_Refresh(void)
{
#if WATCHDOG_ENABLED
    HAL_IWDG_Refresh(&hiwdg);
#endif
}

static void Watchdog_Init(void)
{
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
    hiwdg.Init.Reload = (uint32_t)(WATCHDOG_TIMEOUT_S * 125) - 1;  /* ~16s max */
    hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
    hiwdg.Init.EWI = 0;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
    {
        LOG_ERROR(TAG_BOOT, "IWDG init failed — watchdog NOT active");
    }
    else
    {
        LOG_INFO(TAG_BOOT, "IWDG initialized (%ds timeout)", WATCHDOG_TIMEOUT_S);
    }
}
#endif /* WATCHDOG_ENABLED */

/* ═══════════════════════════════════════════════════════════════════════════
 *  B3 USER Button — Instant Capture + Upload
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * BSP button callback — called from EXTI ISR on B3 rising edge (press).
 * Records the press timestamp; the main loop handles release detection
 * and distinguishes short press (capture) from long press (portal).
 */
void BSP_PB_Callback(Button_TypeDef Button)
{
    if (Button == BUTTON_USER)
    {
        s_button_press_tick = HAL_GetTick();
        s_button_held = 1;
    }
}

/**
 * Perform a single camera capture + HTTP upload cycle.
 * Called from the main loop on B3 short press (< 3s).
 */
static void _do_button_capture(void)
{
    s_button_task_id++;
    uint32_t perf_start = HAL_GetTick();
    LOG_INFO(TAG_BOOT, "=== BUTTON CAPTURE (task_id=%lu) ===",
             (unsigned long)s_button_task_id);

    if (s_ota_in_progress)
    {
        LOG_WARN(TAG_BOOT, "Button capture aborted — OTA in progress");
        return;
    }

    char status_msg[256];
    snprintf(status_msg, sizeof(status_msg), "{\"status\":\"job_received\",\"task_id\":%lu}", (unsigned long)s_button_task_id);
    MQTT_PublishStatus(status_msg);

    uint32_t captured_size = 0;
    CameraStatus_t cam_ret = CAMERA_ERROR_CAPTURE;

    /* Turn on RED LED during image capture */
    BSP_LED_On(LED_RED);

    /* ── Attempt 1: Warm capture (with internal retries) ── */
    if (Camera_IsInitialized())
    {
        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"capturing\",\"task_id\":%lu}", (unsigned long)s_button_task_id);
        MQTT_PublishStatus(status_msg);

        cam_ret = Camera_WarmCapture(
            s_image_buffer, CAMERA_FRAME_BUFFER_SIZE, &captured_size);
    }

    /* ── Attempt 2: Cold-reinit fallback ── */
    if (cam_ret != CAMERA_OK || captured_size == 0)
    {
        LOG_WARN(TAG_CAM, "Warm capture failed (ret=%d) — cold-reinit fallback", cam_ret);
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"camera_reinit\",\"task_id\":%lu}", (unsigned long)s_button_task_id);
        MQTT_PublishStatus(status_msg);

        Camera_DeInit();
        HAL_Delay(100);

        if (Camera_Init(CAMERA_DEFAULT_RESOLUTION) == CAMERA_OK)
        {
            snprintf(status_msg, sizeof(status_msg),
                     "{\"status\":\"capturing\",\"task_id\":%lu,\"fallback\":\"cold\"}", (unsigned long)s_button_task_id);
            MQTT_PublishStatus(status_msg);

            captured_size = 0;
            cam_ret = Camera_CaptureFrame(
                s_image_buffer, CAMERA_FRAME_BUFFER_SIZE, &captured_size);
        }
        else
        {
            LOG_ERROR(TAG_CAM, "Cold reinit failed — capture impossible");
            cam_ret = CAMERA_ERROR_INIT;
        }
    }

    /* ── Final failure — never silent ── */
    if (cam_ret != CAMERA_OK || captured_size == 0)
    {
        LOG_ERROR(TAG_CAM, "Capture failed after all attempts (ret=%d, size=%lu)",
                  cam_ret, (unsigned long)captured_size);
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"error\",\"task_id\":%lu,\"reason\":\"capture_failed\"}",
                 (unsigned long)s_button_task_id);
        MQTT_PublishStatus(status_msg);
        BSP_LED_Off(LED_GREEN);
        BSP_LED_Off(LED_RED);
        return;
    }

    LOG_INFO(TAG_BOOT, "Captured %lu bytes — uploading...",
             (unsigned long)captured_size);

    /* Notify UI that upload is starting */
    snprintf(status_msg, sizeof(status_msg), "{\"status\":\"uploading\",\"task_id\":%lu}", (unsigned long)s_button_task_id);
    MQTT_PublishStatus(status_msg);

    /* Upload via HTTP POST */
    WiFiStatus_t wifi_ret = WiFi_HttpPostImage(
        SERVER_UPLOAD_URL, s_button_task_id,
        s_image_buffer, captured_size);

    uint32_t total_ms = HAL_GetTick() - perf_start;

    if (wifi_ret == WIFI_OK)
    {
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"captured\",\"task_id\":%lu,\"size\":%lu,\"trigger\":\"button\",\"latency_ms\":%lu}",
                 (unsigned long)s_button_task_id,
                 (unsigned long)captured_size,
                 (unsigned long)total_ms);
        LOG_INFO(TAG_BOOT, "[PERF] Button capture complete: %lums total",
                 (unsigned long)total_ms);
    }
    else
    {
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"error\",\"task_id\":%lu,\"reason\":\"upload_failed\",\"trigger\":\"button\"}",
                 (unsigned long)s_button_task_id);
        LOG_ERROR(TAG_BOOT, "Upload FAILED");
    }

    MQTT_PublishStatus(status_msg);

    /* Turn off RED LED after capture cycle completes */
    BSP_LED_Off(LED_RED);

    /* SEC-10: Cryptographic Sanitization */
    secure_erase(s_image_buffer, captured_size);

#if STACK_WATERMARK_ENABLED
    RAM_CheckStackHighWater();
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT "capture_now" — Immediate Single Capture
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_capture_now(void)
{
    uint32_t task_id = s_capture_now_task_id;
    uint32_t perf_start = HAL_GetTick();
    LOG_INFO(TAG_BOOT, "=== CAPTURE NOW (task_id=%lu) ===",
             (unsigned long)task_id);

    if (s_ota_in_progress)
    {
        LOG_WARN(TAG_BOOT, "Capture now aborted — OTA in progress");
        MQTT_PublishStatus("{\"status\":\"error\",\"reason\":\"ota_in_progress\"}");
        return;
    }

    char status_msg[256];
    snprintf(status_msg, sizeof(status_msg), "{\"status\":\"job_received\",\"task_id\":%lu}", (unsigned long)task_id);
    MQTT_PublishStatus(status_msg);

    uint32_t captured_size = 0;
    CameraStatus_t cam_ret = CAMERA_ERROR_CAPTURE;

    /* Turn on RED LED during image capture */
    BSP_LED_On(LED_RED);

    /* ── Attempt 1: Warm capture (with internal retries) ── */
    if (Camera_IsInitialized())
    {
        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"capturing\",\"task_id\":%lu}", (unsigned long)task_id);
        MQTT_PublishStatus(status_msg);

        cam_ret = Camera_WarmCapture(
            s_image_buffer, CAMERA_FRAME_BUFFER_SIZE, &captured_size);
    }

    /* ── Attempt 2: Cold-reinit fallback ──
     * If warm capture failed (or camera was not initialized),
     * fully deinit + reinit the camera and try a cold capture. */
    if (cam_ret != CAMERA_OK || captured_size == 0)
    {
        LOG_WARN(TAG_CAM, "Warm capture failed (ret=%d) — cold-reinit fallback", cam_ret);
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"camera_reinit\",\"task_id\":%lu}", (unsigned long)task_id);
        MQTT_PublishStatus(status_msg);

        /* Full power-cycle of the camera subsystem */
        Camera_DeInit();
        HAL_Delay(200);  /* Let sensor PLL re-lock after power cycle */

        if (Camera_Init(CAMERA_DEFAULT_RESOLUTION) == CAMERA_OK)
        {
            snprintf(status_msg, sizeof(status_msg),
                     "{\"status\":\"capturing\",\"task_id\":%lu,\"fallback\":\"cold\"}", (unsigned long)task_id);
            MQTT_PublishStatus(status_msg);

            captured_size = 0;
            cam_ret = Camera_CaptureFrame(
                s_image_buffer, CAMERA_FRAME_BUFFER_SIZE, &captured_size);
        }
        else
        {
            LOG_ERROR(TAG_CAM, "Cold reinit failed — capture impossible");
            cam_ret = CAMERA_ERROR_INIT;
        }
    }

    /* ── Final failure — never silent ── */
    if (cam_ret != CAMERA_OK || captured_size == 0)
    {
        LOG_ERROR(TAG_CAM, "Capture failed after all attempts (ret=%d, size=%lu)",
                  cam_ret, (unsigned long)captured_size);
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"error\",\"task_id\":%lu,\"reason\":\"capture_failed\"}",
                 (unsigned long)task_id);
        MQTT_PublishStatus(status_msg);
        BSP_LED_Off(LED_GREEN);
        BSP_LED_Off(LED_RED);
        return;
    }

    LOG_INFO(TAG_BOOT, "Captured %lu bytes — uploading...",
             (unsigned long)captured_size);

    snprintf(status_msg, sizeof(status_msg),
             "{\"status\":\"uploading\",\"task_id\":%lu,\"size\":%lu}",
             (unsigned long)task_id, (unsigned long)captured_size);
    MQTT_PublishStatus(status_msg);

    /* Upload via HTTP POST (blocking, but _socket_send_all now
     * calls MQTT_ProcessLoop every 2s to keep the broker alive) */
    WiFiStatus_t wifi_ret = WiFi_HttpPostImage(
        SERVER_UPLOAD_URL, task_id,
        s_image_buffer, captured_size);

    uint32_t total_ms = HAL_GetTick() - perf_start;

    if (wifi_ret == WIFI_OK)
    {
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"captured\",\"task_id\":%lu,\"size\":%lu,\"latency_ms\":%lu,\"trigger\":\"mqtt_capture_now\"}",
                 (unsigned long)task_id, (unsigned long)captured_size, (unsigned long)total_ms);
        LOG_INFO(TAG_BOOT, "[PERF] Capture+upload: %lums", (unsigned long)total_ms);
    }
    else
    {
        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"error\",\"task_id\":%lu,\"reason\":\"upload_failed\"}",
                 (unsigned long)task_id);
        LOG_ERROR(TAG_BOOT, "Upload FAILED");
    }

    MQTT_PublishStatus(status_msg);
    BSP_LED_Off(LED_RED);
    secure_erase(s_image_buffer, captured_size);

#if STACK_WATERMARK_ENABLED
    RAM_CheckStackHighWater();
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT "capture_sequence" — Multiple Timed Captures
 *
 *  Executes captures at precise ms offsets from the moment the command
 *  is processed. Camera stays initialized for the entire sequence to
 *  avoid repeated init/deinit overhead.
 *
 *  JSON format: {"type":"capture_sequence","delays_ms":[0, 2000, 5000]}
 *  → Captures at now+0ms, now+2000ms, now+5000ms
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_capture_sequence(void)
{
    uint32_t count = s_sequence_count;
    uint32_t base_id = s_sequence_base_task_id;

    LOG_INFO(TAG_BOOT, "=== CAPTURE SEQUENCE (%lu captures, base_id=%lu) ===",
             (unsigned long)count, (unsigned long)base_id);

    if (s_ota_in_progress)
    {
        LOG_WARN(TAG_BOOT, "Capture sequence aborted — OTA in progress");
        MQTT_PublishStatus("{\"status\":\"error\",\"reason\":\"ota_in_progress\"}");
        return;
    }

    if (count == 0)
    {
        LOG_WARN(TAG_BOOT, "Empty capture sequence — nothing to do");
        return;
    }

    /* Ensure camera is initialized (should already be warm from boot) */
    if (!Camera_IsInitialized())
    {
        LOG_WARN(TAG_CAM, "Camera cold — initializing for sequence");
        if (Camera_Init(CAMERA_DEFAULT_RESOLUTION) != CAMERA_OK)
        {
            LOG_ERROR(TAG_CAM, "Camera init failed for sequence");
            return;
        }
    }

    s_sequence_abort = 0;

    /* Record the sequence start time for precise ms-offset timing */
    uint32_t sequence_start = HAL_GetTick();

    for (uint32_t i = 0; i < count; i++)
    {
        /* Check for abort (set by delete_schedule command) */
        if (s_sequence_abort)
        {
            LOG_WARN(TAG_BOOT, "Seq[%lu]: ABORTED by delete_schedule", (unsigned long)i);
            MQTT_PublishStatus("{\"status\":\"sequence_aborted\"}");
            break;
        }

        uint32_t task_id = base_id + i;
        uint32_t target_ms = s_sequence_delays_ms[i];

        /* Wait until the target offset from sequence start.
         * Poll in small steps so we can check for abort during waits. */
        uint32_t now = HAL_GetTick();
        uint32_t elapsed = now - sequence_start;
        while (elapsed < target_ms && !s_sequence_abort)
        {
            uint32_t remaining = target_ms - elapsed;
            uint32_t step = (remaining > 200) ? 200 : remaining;
            if (i == 0 && elapsed == (now - sequence_start))
            {
                LOG_DEBUG(TAG_BOOT, "Seq[%lu]: waiting %lums for offset %lums...",
                          (unsigned long)i, (unsigned long)remaining, (unsigned long)target_ms);
            }
            HAL_Delay(step);
            elapsed = HAL_GetTick() - sequence_start;
        }
        if (s_sequence_abort) continue;  /* Will be caught at top of next iteration */

        /* Use warm capture — sensor is already converged */
        BSP_LED_On(LED_RED);
        
        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"capturing\",\"task_id\":%lu}", (unsigned long)task_id);
        MQTT_PublishStatus(status_msg);

        uint32_t captured_size = 0;
        CameraStatus_t cam_ret = Camera_WarmCapture(
            s_image_buffer, CAMERA_FRAME_BUFFER_SIZE, &captured_size);
        uint32_t capture_tick = HAL_GetTick() - sequence_start;

        if (cam_ret != CAMERA_OK || captured_size == 0)
        {
            LOG_ERROR(TAG_CAM, "Seq[%lu]: capture failed (ret=%d)",
                      (unsigned long)i, cam_ret);
            BSP_LED_Off(LED_RED);
            continue;  /* Skip to next capture in sequence */
        }

        LOG_INFO(TAG_BOOT, "Seq[%lu]: captured %lu bytes at %lums (target=%lums)",
                 (unsigned long)i,
                 (unsigned long)captured_size,
                 (unsigned long)capture_tick,
                 (unsigned long)target_ms);

        /* Notify UI that upload is starting */
        snprintf(status_msg, sizeof(status_msg), "{\"status\":\"uploading\",\"task_id\":%lu}", (unsigned long)task_id);
        MQTT_PublishStatus(status_msg);

        /* Upload immediately */
        WiFiStatus_t wifi_ret = WiFi_HttpPostImage(
            SERVER_UPLOAD_URL, task_id,
            s_image_buffer, captured_size);

        snprintf(status_msg, sizeof(status_msg),
                 "{\"status\":\"captured\",\"task_id\":%lu,\"size\":%lu,"
                 "\"trigger\":\"sequence\",\"seq_index\":%lu,\"seq_total\":%lu,"
                 "\"actual_offset_ms\":%lu,\"target_offset_ms\":%lu}",
                 (unsigned long)task_id,
                 (unsigned long)captured_size,
                 (unsigned long)i,
                 (unsigned long)count,
                 (unsigned long)capture_tick,
                 (unsigned long)target_ms);

        MQTT_PublishStatus(status_msg);

        if (wifi_ret != WIFI_OK)
        {
            LOG_WARN(TAG_BOOT, "Seq[%lu]: upload failed — continuing sequence", (unsigned long)i);
        }

        /* Turn off RED LED after upload completes */
        BSP_LED_Off(LED_RED);
    }

    /* Do NOT deinit camera — keep warm for next capture */
    
    /* SEC-10: Cryptographic Sanitization */
    secure_erase(s_image_buffer, CAMERA_FRAME_BUFFER_SIZE);

    LOG_INFO(TAG_BOOT, "[PERF] Sequence complete — %lu captures in %lums",
             (unsigned long)count,
             (unsigned long)(HAL_GetTick() - sequence_start));

#if STACK_WATERMARK_ENABLED
    RAM_CheckStackHighWater();
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MQTT "firmware_update" — OTA Update Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_ota_update(void)
{
    LOG_INFO(TAG_OTA, "=== FIRMWARE UPDATE REQUESTED ===");
    MQTT_PublishStatus("{\"status\":\"ota_checking\"}");

    s_ota_in_progress = 1;

    OTAVersionInfo_t info;
    OTAStatus_t status = OTA_CheckForUpdate(&info);

    if (status == OTA_NO_UPDATE)
    {
        LOG_INFO(TAG_OTA, "Already up-to-date (v%s)", FW_VERSION);
        MQTT_PublishStatus("{\"status\":\"ota_up_to_date\",\"firmware\":\"" FW_VERSION "\"}");
        s_ota_in_progress = 0;
        return;
    }

    if (status != OTA_OK)
    {
        LOG_ERROR(TAG_OTA, "Version check failed (err=%d)", status);
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "{\"status\":\"ota_error\",\"reason\":\"version_check_failed\",\"code\":%d}",
                 status);
        MQTT_PublishStatus(msg);
        s_ota_in_progress = 0;
        return;
    }

    /* Update available — download and flash */
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"status\":\"ota_downloading\",\"new_version\":\"%s\",\"size\":%lu}",
             info.version, (unsigned long)info.size);
    MQTT_PublishStatus(msg);

    /* ── Hyper-distinct Update Received Pattern ── */
    /* 5 rapid RED+GREEN strobe flashes before freezing for Flash Erase (Flushing) */
    for (int i = 0; i < 5; i++) {
        BSP_LED_On(LED_RED);
        BSP_LED_On(LED_GREEN);
        HAL_Delay(80);
        BSP_LED_Off(LED_RED);
        BSP_LED_Off(LED_GREEN);
        HAL_Delay(80);
    }

    /* ═════════════════════════════════════════════════════════════════════
     * CRITICAL: MQTT Quiesce Before Download
     *
     * The EMW3080 SPI transport shares a single bus for ALL socket traffic.
     * MQTT keepalive frames compete with HTTP download data, causing the
     * SPI command queue to stall and hit MX_WIFI_CMD_TIMEOUT.
     *
     * Solution: Disconnect MQTT → drain SPI queue → download in isolation
     * → reconnect MQTT after flashing.
     * ═════════════════════════════════════════════════════════════════════ */
    LOG_INFO(TAG_OTA, "Quiescing MQTT and Camera to isolate buses for download...");
    MQTT_Disconnect();

    if (Camera_IsInitialized())
    {
        LOG_INFO(TAG_OTA, "Stopping Camera DCMI DMA before reusing frame buffer...");
        if (Camera_DeInit() != CAMERA_OK) {
            LOG_ERROR(TAG_OTA, "CRITICAL: Camera de-init FAILED — aborting OTA for safety");
            MQTT_PublishStatus("{\"status\":\"ota_error\",\"reason\":\"camera_deinit_failed\"}");
            MQTTConfig_t re_cfg = {
                .broker_host = MQTT_BROKER_HOST,
                .broker_port = MQTT_BROKER_PORT,
                .client_id   = MQTT_CLIENT_ID,
            };
            MQTT_Init(&re_cfg);
            MQTT_SubscribeCommands(on_command_received);
            s_ota_in_progress = 0;
            return;
        }
    }

    /* Drain any residual SPI traffic from the MQTT disconnect */
    for (int drain = 0; drain < 10; drain++)
    {
        MX_WIFI_IO_YIELD(wifi_obj_get(), 200);
#if WATCHDOG_ENABLED
        IWDG->KR = 0x0000AAAAu;
#endif
    }
    LOG_INFO(TAG_OTA, "MQTT disconnected — SPI bus dedicated to OTA");

#if STACK_WATERMARK_ENABLED
    LOG_INFO(TAG_OTA, "Stack usage before OTA: %lu bytes", (unsigned long)RAM_CheckStackHighWater());
#endif

    status = OTA_DownloadAndFlash(&info, s_image_buffer, sizeof(s_image_buffer));

#if STACK_WATERMARK_ENABLED
    LOG_INFO(TAG_OTA, "Stack usage after OTA: %lu bytes", (unsigned long)RAM_CheckStackHighWater());
#endif

    if (status != OTA_OK)
    {
        LOG_ERROR(TAG_OTA, "Download/flash failed (err=%d) — reconnecting MQTT", status);

        /* Reconnect MQTT so the board stays manageable */
        MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);
        MQTTConfig_t re_cfg = {
            .broker_host = MQTT_BROKER_HOST,
            .broker_port = MQTT_BROKER_PORT,
            .client_id   = MQTT_CLIENT_ID,
        };
        MQTT_Init(&re_cfg);
        MQTT_SubscribeCommands(on_command_received);

        snprintf(msg, sizeof(msg),
                 "{\"status\":\"ota_error\",\"reason\":\"flash_failed\",\"code\":%d}",
                 status);
        MQTT_PublishStatus(msg);

        /* Restore camera state since payload failed and we are resuming normal ops */
        if (!Camera_IsInitialized())
        {
            LOG_INFO(TAG_OTA, "Restoring Camera state after failed OTA...");
            Camera_Init(CAMERA_DEFAULT_RESOLUTION);
        }

        s_ota_in_progress = 0;
        return;
    }

    /* Success — publish reboot notice via fresh MQTT, then swap banks */
    MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);
    {
        MQTTConfig_t re_cfg = {
            .broker_host = MQTT_BROKER_HOST,
            .broker_port = MQTT_BROKER_PORT,
            .client_id   = MQTT_CLIENT_ID,
        };
        MQTT_Init(&re_cfg);
        MQTT_SubscribeCommands(on_command_received);
    }

    MQTT_PublishStatus("{\"status\":\"ota_rebooting\"}");
    HAL_Delay(200);  /* Let MQTT message send before reset */

    OTA_SwapBankAndReset();

    /* If we get here, swap failed */
    MQTT_PublishStatus("{\"status\":\"ota_error\",\"reason\":\"bank_swap_failed\"}");
    s_ota_in_progress = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WiFi Runtime Reconfiguration
 *
 *  Tears down MQTT → WiFi → reconnects with new credentials → re-establishes
 *  MQTT subscription. The board publishes wifi_reconfigured/wifi_reconfig_failed
 *  as ACK to the dashboard.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_wifi_reconfig(void)
{
    LOG_INFO(TAG_WIFI, "=== WIFI RECONFIGURATION ===");
    LOG_INFO(TAG_WIFI, "New SSID: '%s'", s_runtime_ssid);

    /* ── Step 1: Tear down MQTT and WiFi ──────────────── */
    MQTT_Disconnect();
    MX_WIFI_Disconnect(wifi_obj_get());

    /* SPI pipeline flush */
    MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);

#if WATCHDOG_ENABLED
    HAL_IWDG_Refresh(&hiwdg);
#endif

    /* ── Step 2: Reconnect with new credentials ──────── */
    WiFiStatus_t ret = WiFi_Connect(s_runtime_ssid, s_runtime_password);

    if (ret != WIFI_OK)
    {
        LOG_ERROR(TAG_WIFI, "WiFi reconnect FAILED with new creds. Rebooting to retry or open portal...");
        NVIC_SystemReset();
    }

#if WATCHDOG_ENABLED
    HAL_IWDG_Refresh(&hiwdg);
#endif

    /* ── Step 3: Re-establish MQTT ────────────────────── */
    MQTTConfig_t mqtt_cfg = {
        .broker_host = MQTT_BROKER_HOST,
        .broker_port = MQTT_BROKER_PORT,
        .client_id   = MQTT_CLIENT_ID,
    };

    MX_WIFI_IO_YIELD(wifi_obj_get(), 2000);

    if (MQTT_Init(&mqtt_cfg) != 0)
    {
        LOG_ERROR(TAG_MQTT, "MQTT reconnect failed after WiFi reconfig");
        return;
    }

    MQTT_SubscribeCommands(on_command_received);

    /* ── Step 4: ACK to dashboard ────────────────────── */
    if (s_has_runtime_wifi)
    {
        char ack[128];
        snprintf(ack, sizeof(ack),
                 "{\"status\":\"wifi_reconfigured\",\"ssid\":\"%s\",\"firmware\":\"" FW_VERSION "\"}",
                 s_runtime_ssid);
        MQTT_PublishStatus(ack);
        LOG_INFO(TAG_WIFI, "WiFi reconfigured OK — connected to '%s'", s_runtime_ssid);
    }
    else
    {
        MQTT_PublishStatus("{\"status\":\"wifi_reconfig_failed\",\"reason\":\"reverted_to_default\"}");
        LOG_WARN(TAG_WIFI, "WiFi reconfig failed — reverted to compile-time defaults");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Ping Sequence Handler
 * 
 *  Flashes RED 3 times and GREEN 3 times.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_ping_sequence(void)
{
    LOG_INFO(TAG_BOOT, "=== PING SEQUENCE ===");
    
    BSP_LED_Off(LED_GREEN);
    BSP_LED_Off(LED_RED);
    
    /* 3x RED */
    for (int i = 0; i < 3; i++) {
        BSP_LED_On(LED_RED);
        HAL_Delay(150);
        BSP_LED_Off(LED_RED);
        HAL_Delay(100);
    }
    
    HAL_Delay(200);
    
    /* 3x GREEN */
    for (int i = 0; i < 3; i++) {
        BSP_LED_On(LED_GREEN);
        HAL_Delay(150);
        BSP_LED_Off(LED_GREEN);
        HAL_Delay(100);
    }
    
    MQTT_PublishStatus("{\"status\":\"ping_complete\"}");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Captive Portal Handler (MQTT-triggered)
 *
 *  Tears down current connectivity and enters portal mode.
 *  The board reboots after the user configures new WiFi credentials.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _do_start_portal(void)
{
    LOG_INFO(TAG_PORT, "=== MQTT-TRIGGERED PORTAL MODE ===");

    /* Graceful MQTT/WiFi teardown */
    MQTT_Disconnect();
    MX_WIFI_Disconnect(wifi_obj_get());
    MX_WIFI_IO_YIELD(wifi_obj_get(), 1000);

    /* Re-init WiFi module for AP mode */
    WiFi_DeInit();
    HAL_Delay(1000);

    if (WiFi_Init() != WIFI_OK)
    {
        LOG_ERROR(TAG_PORT, "WiFi re-init failed for portal mode");
        NVIC_SystemReset();  /* Hard reboot as last resort */
    }

    CaptivePortal_Start();  /* Blocks until configured + auto-reboots */
    /* Never returns */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PS-REST — STOP2 + WiFi-associated power-save poll cycle
 *
 *  Called from the main loop when s_lp_mode == 1 and no pending work exists.
 *  WiFi stays associated (802.11 power-save mode) so the board remains
 *  agent-reachable: the MQTT broker holds QoS-0 publishes while we sleep,
 *  and EXTI14 (NOTIFY line) or EXTI13 (B3 button) can break STOP2 early.
 *
 *  Guard: skipped if MQTT is disconnected — keeps the board awake for
 *  reconnect recovery.
 *
 *  On return the main-loop top calls MQTT_ProcessLoop() which drains any
 *  pending RX that accumulated during the sleep interval.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void EnterPSRest(void)
{
    /* Guard: if MQTT is not up, stay awake for reconnect recovery */
    if (!MQTT_IsConnected())
    {
        LOG_WARN(TAG_PWR, "PS-REST: MQTT disconnected — skipping sleep");
        HAL_Delay(SCHEDULER_MQTT_POLL_MS);
        return;
    }

#if WIFI_POWERSAVE_ENABLED
    /* 802.11 power-save: WiFi stays ASSOCIATED, EMW3080 DTIM-sleeps. */
    MX_WIFI_station_powersave(wifi_obj_get(), 1);
#endif

    /* Arm RTC Alarm A REST_POLL_S from now (keepalive/poll). STOP2 also wakes
     * early on the NOTIFY/PD14 (EXTI14) line when a command arrives -> the
     * RTC alarm is the floor, NOTIFY gives sub-second wake-on-ping. */
    if (Scheduler_SetShortAlarm(REST_POLL_S) != 0)
    {
        LOG_ERROR(TAG_PWR, "PS-REST: SetShortAlarm failed — active wait");
#if WIFI_POWERSAVE_ENABLED
        MX_WIFI_station_powersave(wifi_obj_get(), 0);
#endif
        HAL_Delay(SCHEDULER_MQTT_POLL_MS);
        return;
    }

    uint32_t btn_tick_before = s_button_press_tick;

    /* STOP2 sleep.  Note: SysTick (HAL_GetTick) is frozen during STOP2 and is
     * NOT corrected for sleep duration on wake (Scheduler_EnterLowPower only
     * restores the clock frequency).  Therefore PS-REST sleep time is NOT
     * measured here per-cycle; it is derived at energy-publish time as
     * (RTC wall-clock elapsed) - (HAL_GetTick awake elapsed).  See the energy
     * report block in the main loop. */
    Scheduler_EnterLowPower();   /* STOP2: wakes on Alarm A / NOTIFY EXTI14 / B3 */

#if WATCHDOG_ENABLED
    HAL_IWDG_Refresh(&hiwdg);    /* IWDG resumes on STOP2 exit — refresh now */
#endif

#if WIFI_POWERSAVE_ENABLED
    /* Back to full power so MQTT_ProcessLoop services pending RX promptly */
    MX_WIFI_station_powersave(wifi_obj_get(), 0);
#endif

    /* B3 override: physical press during STOP2 exits PS-REST mode */
    if (s_button_press_tick != btn_tick_before)
    {
        s_lp_mode = 0;
        s_button_held = 0;
        LOG_WARN(TAG_PWR, "PS-REST: B3 pressed during sleep — LP mode DISABLED");
        MQTT_PublishStatus("{\"status\":\"awake\",\"reason\":\"button_override\"}");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTC Alarm Callback (called from ISR via HAL)
 * ═══════════════════════════════════════════════════════════════════════════ */

void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc_ptr)
{
    (void)hrtc_ptr;
    /* Wake-up is handled by hardware — execution resumes after WFI in
     * Scheduler_EnterLowPower(). This callback is for any additional
     * housekeeping. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Assert Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    LOG_ERROR(TAG_BOOT, "ASSERT at %s:%lu", file, (unsigned long)line);
    while (1) {
        /* SEC-06: Refresh watchdog to prevent debugger spinning loop from being reset */
#if WATCHDOG_ENABLED
        HAL_IWDG_Refresh(&hiwdg);
#endif
    }
}
#endif
