/**
 * @file    scheduler.h
 * @brief   RTC Alarm-based Task Scheduler for AI-planned monitoring
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Parses JSON schedules from the cloud server and configures RTC alarms
 * to wake the MCU from STOP2/Standby mode at scheduled capture times.
 */

#ifndef __SCHEDULER_H
#define __SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of scheduled tasks */
#define MAX_SCHEDULED_TASKS 32

/* Task actions */
typedef enum {
    ACTION_CAPTURE_IMAGE = 0,
    ACTION_SLEEP,
} TaskAction_t;

/* Single scheduled task */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    TaskAction_t action;
    uint32_t task_id;
    char objective[64];
} ScheduledTask_t;

/* Schedule container */
typedef struct {
    ScheduledTask_t tasks[MAX_SCHEDULED_TASKS];
    uint8_t task_count;
    uint8_t current_index;
} Schedule_t;

/**
 * @brief  Parse a JSON schedule string into the schedule structure.
 * @param  schedule: Pointer to Schedule_t to populate.
 * @param  json_str: Raw JSON string from MQTT.
 * @retval 0 on success, -1 on parse error.
 */
int Scheduler_ParseJSON(Schedule_t *schedule, const char *json_str);

/**
 * @brief  Configure the next RTC alarm based on the current schedule.
 * @retval 0 on success, 1 if no more tasks remain.
 */
int Scheduler_SetNextAlarm(Schedule_t *schedule);

/**
 * @brief  Get the current task to execute (after waking from alarm).
 * @retval Pointer to the current ScheduledTask_t, or NULL if none.
 */
const ScheduledTask_t *Scheduler_GetCurrentTask(Schedule_t *schedule);

/**
 * @brief  Enter low-power mode (STOP2) until the next RTC alarm.
 */
void Scheduler_EnterLowPower(void);

/**
 * @brief  Set RTC Alarm A to fire `seconds` from the current RTC time.
 *         Handles second/minute/hour rollover correctly (BIN format).
 *         Intended for short PS-REST poll wakes.
 * @param  seconds: Wake-up offset in seconds (1..3599).
 * @retval 0 on success, -1 on HAL failure.
 */
int Scheduler_SetShortAlarm(uint32_t seconds);

#endif /* __SCHEDULER_H */
