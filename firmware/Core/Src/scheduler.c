/**
 * @file    scheduler.c
 * @brief   RTC Alarm-based Task Scheduler with JSON Parsing
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Parses AI-generated JSON schedules (received via MQTT) and configures
 * RTC alarms to wake the MCU from STOP2 mode at each scheduled capture time.
 *
 * JSON format (from the planning engine):
 *   {"tasks": [
 *     {"time": "15:00", "action": "CAPTURE_IMAGE", "id": 1, "objective": "Check occupancy"},
 *     {"time": "15:15", "action": "CAPTURE_IMAGE", "id": 2, "objective": "Count people"}
 *   ]}
 *
 * Dependencies: cJSON (Dave Gamble's single-file JSON parser)
 */

#include "scheduler.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "main.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTC Handle (shared with main.c via extern)
 * ═══════════════════════════════════════════════════════════════════════════ */

extern RTC_HandleTypeDef hrtc;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Convert an action string to the TaskAction_t enum.
 */
static TaskAction_t _parse_action(const char *action_str)
{
    if (action_str == NULL)                       return ACTION_CAPTURE_IMAGE;
    if (strcmp(action_str, "CAPTURE_IMAGE") == 0)  return ACTION_CAPTURE_IMAGE;
    if (strcmp(action_str, "SLEEP") == 0)          return ACTION_SLEEP;
    return ACTION_CAPTURE_IMAGE;  /* Default fallback */
}

/**
 * Simple insertion sort by (hour, minute, second).
 * We sort after parsing so that Scheduler_SetNextAlarm() can iterate linearly.
 */
static void _sort_tasks_by_time(Schedule_t *schedule)
{
    for (int i = 1; i < schedule->task_count; i++)
    {
        ScheduledTask_t tmp = schedule->tasks[i];
        int j = i - 1;
        /* Compare (hour, minute, second) tuples */
        while (j >= 0)
        {
            int32_t a = (int32_t)schedule->tasks[j].hour * 3600
                      + (int32_t)schedule->tasks[j].minute * 60
                      + (int32_t)schedule->tasks[j].second;
            int32_t b = (int32_t)tmp.hour * 3600
                      + (int32_t)tmp.minute * 60
                      + (int32_t)tmp.second;
            if (a <= b) break;
            schedule->tasks[j + 1] = schedule->tasks[j];
            j--;
        }
        schedule->tasks[j + 1] = tmp;
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  JSON Schedule Parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

int Scheduler_ParseJSON(Schedule_t *schedule, const char *json_str)
{
    if (schedule == NULL || json_str == NULL)
    {
        LOG_ERROR(TAG_SCHED, "NULL argument to ParseJSON");
        return -1;
    }

    /* ── Input size guard ──
     * Enterprise practice: reject absurdly large payloads before they
     * reach the JSON parser's malloc calls. */
    uint32_t json_len = (uint32_t)strlen(json_str);
    if (json_len > SCHEDULE_JSON_MAX)
    {
        LOG_ERROR(TAG_SCHED, "Schedule JSON too large (%lu > %d), rejecting",
                  (unsigned long)json_len, SCHEDULE_JSON_MAX);
        return -1;
    }

    /* Reset schedule */
    memset(schedule, 0, sizeof(Schedule_t));

    LOG_DEBUG(TAG_SCHED, "Parsing schedule JSON (%u bytes)...",
              (unsigned)json_len);

    /* Parse JSON */
    json_mem_reset();
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        LOG_ERROR(TAG_SCHED, "JSON parse error near: %.20s",
                  error_ptr ? error_ptr : "unknown");
        return -1;
    }

    /* Extract the "tasks" array */
    cJSON *tasks_array = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (!cJSON_IsArray(tasks_array))
    {
        LOG_ERROR(TAG_SCHED, "JSON missing 'tasks' array");
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(tasks_array);
    if (count > MAX_SCHEDULED_TASKS)
    {
        LOG_WARN(TAG_SCHED, "Schedule has %d tasks, truncating to %d",
                 count, MAX_SCHEDULED_TASKS);
        count = MAX_SCHEDULED_TASKS;
    }

    /* Parse each task */
    int idx = 0;
    cJSON *task_json = NULL;
    cJSON_ArrayForEach(task_json, tasks_array)
    {
        if (idx >= count) break;

        ScheduledTask_t *task = &schedule->tasks[idx];

        /* Parse "time" — format "HH:MM:SS" (also accepts "HH:MM") */
        cJSON *time_item = cJSON_GetObjectItemCaseSensitive(task_json, "time");
        if (cJSON_IsString(time_item) && time_item->valuestring != NULL)
        {
            int h = 0, m = 0, s = 0;
            int n = sscanf(time_item->valuestring, "%d:%d:%d", &h, &m, &s);
            if (n >= 2)
            {
                /* SEC-04: clamp to valid RTC ranges */
                if (h < 0 || h > 23) h = 0;
                if (m < 0 || m > 59) m = 0;
                if (s < 0 || s > 59) s = 0;
                task->hour   = (uint8_t)h;
                task->minute = (uint8_t)m;
                task->second = (n >= 3) ? (uint8_t)s : 0;
            }
        }

        /* Parse "action" */
        cJSON *action_item = cJSON_GetObjectItemCaseSensitive(task_json, "action");
        task->action = _parse_action(
            cJSON_IsString(action_item) ? action_item->valuestring : NULL);

        /* Parse "id" */
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(task_json, "id");
        if (cJSON_IsNumber(id_item))
        {
            task->task_id = (uint32_t)id_item->valuedouble;
        }

        /* Parse "objective" */
        cJSON *obj_item = cJSON_GetObjectItemCaseSensitive(task_json, "objective");
        if (cJSON_IsString(obj_item) && obj_item->valuestring != NULL)
        {
            strncpy(task->objective, obj_item->valuestring,
                    sizeof(task->objective) - 1);
            task->objective[sizeof(task->objective) - 1] = '\0';
        }

        LOG_DEBUG(TAG_SCHED, "  Task %d: %02d:%02d:%02d action=%d obj='%.32s'",
                  task->task_id, task->hour, task->minute, task->second,
                  task->action, task->objective);

        idx++;
    }

    schedule->task_count   = (uint8_t)idx;
    schedule->current_index = 0;

    cJSON_Delete(root);

    /* Sort by time so we can iterate linearly */
    _sort_tasks_by_time(schedule);

    LOG_INFO(TAG_SCHED, "Parsed %d tasks from schedule", schedule->task_count);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTC Alarm Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

int Scheduler_SetNextAlarm(Schedule_t *schedule)
{
    if (schedule == NULL)
        return 1;

    /* ── Skip past tasks ──
     * When a schedule arrives late (network delay, boot time), some tasks
     * may already be in the past.  Advance current_index past any task
     * that is more than 30 s ago so the board doesn't sleep until tomorrow.
     * Uses raw (unwrapped) diff to correctly distinguish past from future. */
    RTC_TimeTypeDef now_time;
    RTC_DateTypeDef now_date;
    HAL_RTC_GetTime(&hrtc, &now_time, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &now_date, RTC_FORMAT_BIN);  /* Must read date after time per RM */

    int32_t now_secs = (int32_t)now_time.Hours * 3600
                     + (int32_t)now_time.Minutes * 60
                     + (int32_t)now_time.Seconds;

    while (schedule->current_index < schedule->task_count)
    {
        const ScheduledTask_t *t = &schedule->tasks[schedule->current_index];
        int32_t t_secs = (int32_t)t->hour * 3600
                       + (int32_t)t->minute * 60
                       + (int32_t)t->second;
        int32_t raw_diff = t_secs - now_secs;

        /* Midnight crossing: now=23:59, task=00:05 → raw_diff=-86340 → +86400 = 360 */
        if (raw_diff < -43200)
            raw_diff += 86400;

        /* Keep tasks in the future or recently missed (≤30 s ago) */
        if (raw_diff >= -30)
            break;

        /* Task is stale (> 30 s in the past) — skip */
        LOG_WARN(TAG_SCHED, "Skipping past task %u at %02u:%02u:%02u (%ld s ago)",
                 t->task_id, t->hour, t->minute, t->second,
                 (long)(-raw_diff));
        schedule->current_index++;
    }

    if (schedule->current_index >= schedule->task_count)
    {
        LOG_INFO(TAG_SCHED, "All %d tasks completed — no more alarms",
                 schedule->task_count);
        return 1;  /* No more tasks */
    }

    const ScheduledTask_t *next = &schedule->tasks[schedule->current_index];

    LOG_INFO(TAG_SCHED, "Setting RTC alarm for task %u at %02u:%02u:%02u",
             next->task_id, next->hour, next->minute, next->second);

    /* Re-read RTC (may have advanced during skip loop) */
    HAL_RTC_GetTime(&hrtc, &now_time, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &now_date, RTC_FORMAT_BIN);

    now_secs = (int32_t)now_time.Hours * 3600
             + (int32_t)now_time.Minutes * 60
             + (int32_t)now_time.Seconds;
    int32_t task_secs = (int32_t)next->hour * 3600
                      + (int32_t)next->minute * 60
                      + (int32_t)next->second;
    int32_t diff = task_secs - now_secs;

    LOG_DEBUG(TAG_SCHED, "Current RTC: %02u:%02u:%02u, task: %02u:%02u:%02u (diff=%ld s)",
              now_time.Hours, now_time.Minutes, now_time.Seconds,
              next->hour, next->minute, next->second, (long)diff);

    if (diff < 0)
    {
        diff += 86400;
    }

    /* Target is within a 30-second window: execute immediately */
    if (diff <= 30 || diff >= 86370)
    {
        LOG_INFO(TAG_SCHED, "Task %u time is NOW — executing immediately",
                 next->task_id);
        return 2;  /* Execute immediately, don't sleep */
    }

    /* Configure RTC Alarm A to fire at the specified time (including seconds) */
    RTC_AlarmTypeDef alarm = {0};
    alarm.AlarmTime.Hours   = next->hour;
    alarm.AlarmTime.Minutes = next->minute;
    alarm.AlarmTime.Seconds = next->second;
    alarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    alarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;

    /* Mask: match on Hours, Minutes, and Seconds (ignore date/day) */
    alarm.AlarmMask       = RTC_ALARMMASK_DATEWEEKDAY;
    alarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
    alarm.Alarm           = RTC_ALARM_A;

    /* Disable any previous alarm first */
    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);

    if (HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN) != HAL_OK)
    {
        LOG_ERROR(TAG_SCHED, "Failed to set RTC alarm");
        return -1;
    }

    LOG_INFO(TAG_SCHED, "Alarm set — will wake at %02u:%02u:%02u",
             next->hour, next->minute, next->second);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Task Retrieval
 * ═══════════════════════════════════════════════════════════════════════════ */

const ScheduledTask_t *Scheduler_GetCurrentTask(Schedule_t *schedule)
{
    if (schedule == NULL ||
        schedule->current_index >= schedule->task_count)
    {
        return NULL;
    }

    const ScheduledTask_t *task = &schedule->tasks[schedule->current_index];
    schedule->current_index++;

    LOG_INFO(TAG_SCHED, "Executing task %u: %02u:%02u '%s'",
             task->task_id, task->hour, task->minute, task->objective);

    return task;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Low Power — STOP2 Mode Entry
 * ═══════════════════════════════════════════════════════════════════════════ */

void Scheduler_EnterLowPower(void)
{
#if LOW_POWER_MODE_ENABLED

    LOG_INFO(TAG_PWR, "Entering STOP2 mode — waiting for RTC alarm...");

    /* Small delay to flush UART before sleeping */
    HAL_Delay(10);

    /* Suspend SysTick to prevent wake from tick interrupt */
    HAL_SuspendTick();

    /* Clear all wake-up flags */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_STOPF);

    /*
     * Enter STOP2 mode:
     *   - Core clock stopped
     *   - SRAM contents preserved
     *   - RTC continues running on LSE
     *   - Wake source: RTC Alarm A interrupt
     */
    HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

    /* ── Execution resumes here after RTC alarm wake-up ── */

    /* Resume SysTick */
    HAL_ResumeTick();

    /* Re-configure system clock (lost during STOP2) */
    extern void SystemClock_Config(void);
    SystemClock_Config();

    LOG_INFO(TAG_PWR, "Woke from STOP2 — clock restored");

#else
    /* Active wait mode (for debugging) — just delay until alarm fires */
    LOG_INFO(TAG_PWR, "Low-power disabled — active waiting...");

    RTC_TimeTypeDef time_now;
    HAL_RTC_GetTime(&hrtc, &time_now, RTC_FORMAT_BIN);
    LOG_DEBUG(TAG_PWR, "Current time: %02d:%02d:%02d",
              time_now.Hours, time_now.Minutes, time_now.Seconds);

    /* Spin-wait until RTC alarm callback fires */
    while (1)
    {
        HAL_Delay(1000);

        /* Check if alarm fired (the alarm ISR will set the flag) */
        HAL_RTC_GetTime(&hrtc, &time_now, RTC_FORMAT_BIN);
        LOG_DEBUG(TAG_PWR, "Waiting... %02d:%02d:%02d",
                  time_now.Hours, time_now.Minutes, time_now.Seconds);

        /* Break if the alarm handler set a flag (checked in main loop) */
        break;  /* In debug mode, proceed immediately */
    }
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Short-interval RTC Alarm — PS-REST poll wake
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Set RTC Alarm A to fire `seconds` from the current RTC time.
 *
 * Mirrors Scheduler_SetNextAlarm() in alarm-config style (HAL_RTC_SetAlarm_IT,
 * RTC_ALARMMASK_DATEWEEKDAY, RTC_FORMAT_BIN) but computes the target time
 * by adding an offset to the current clock reading rather than using a
 * fixed schedule entry.  Handles second, minute, and hour rollover in
 * pure BIN arithmetic (no BCD conversion required since we use BIN format
 * throughout).
 *
 * @param  seconds  Offset from now in seconds (practical range 1..3599).
 * @retval 0 on success, -1 on HAL error.
 */
int Scheduler_SetShortAlarm(uint32_t seconds)
{
    RTC_TimeTypeDef now_time;
    RTC_DateTypeDef now_date;

    /* Read current RTC time (BIN format; must read date afterwards per RM) */
    HAL_RTC_GetTime(&hrtc, &now_time, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &now_date, RTC_FORMAT_BIN);

    /* Convert current time to seconds-of-day, add offset, wrap at 86400 */
    uint32_t now_secs = (uint32_t)now_time.Hours   * 3600u
                      + (uint32_t)now_time.Minutes * 60u
                      + (uint32_t)now_time.Seconds;
    uint32_t target_secs = (now_secs + seconds) % 86400u;

    /* Decompose back to H:M:S */
    uint8_t target_h = (uint8_t)(target_secs / 3600u);
    uint8_t target_m = (uint8_t)((target_secs % 3600u) / 60u);
    uint8_t target_s = (uint8_t)(target_secs % 60u);

    LOG_DEBUG(TAG_PWR, "SetShortAlarm: now=%02u:%02u:%02u + %lus = %02u:%02u:%02u",
              now_time.Hours, now_time.Minutes, now_time.Seconds,
              (unsigned long)seconds, target_h, target_m, target_s);

    /* Configure and arm Alarm A — mirror SetNextAlarm() style exactly */
    RTC_AlarmTypeDef alarm = {0};
    alarm.AlarmTime.Hours          = target_h;
    alarm.AlarmTime.Minutes        = target_m;
    alarm.AlarmTime.Seconds        = target_s;
    alarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    alarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
    alarm.AlarmMask                = RTC_ALARMMASK_DATEWEEKDAY;
    alarm.AlarmSubSecondMask       = RTC_ALARMSUBSECONDMASK_ALL;
    alarm.Alarm                    = RTC_ALARM_A;

    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);

    if (HAL_RTC_SetAlarm_IT(&hrtc, &alarm, RTC_FORMAT_BIN) != HAL_OK)
    {
        LOG_ERROR(TAG_PWR, "SetShortAlarm: HAL_RTC_SetAlarm_IT failed");
        return -1;
    }

    LOG_INFO(TAG_PWR, "Short alarm set — PS-REST wake at %02u:%02u:%02u",
             target_h, target_m, target_s);
    return 0;
}
