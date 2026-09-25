#include "timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <curses.h>

CountdownTimer g_timers[TIMER_COUNT];

void
InitTimers(void)
{
    static const char *labels[TIMER_COUNT] = {
        "Development", "Stop bath", "Fix"
    };
    static const int has_alarm[TIMER_COUNT]  = { 1, 0, 1 };
    static const int has_temp[TIMER_COUNT]   = { 1, 0, 0 };
    static const int has_recipe[TIMER_COUNT] = { 1, 0, 0 };
    int i;

    for (i = 0; i < TIMER_COUNT; i++) {
        CountdownTimer *t = &g_timers[i];
        memset(t, 0, sizeof(*t));
        t->label      = labels[i];
        t->has_alarm  = has_alarm[i];
        t->has_temp   = has_temp[i];
        t->has_recipe = has_recipe[i];
        snprintf(t->hh_buf,        sizeof(t->hh_buf),        "00");
        snprintf(t->mm_buf,        sizeof(t->mm_buf),        "09");
        snprintf(t->ss_buf,        sizeof(t->ss_buf),        "30");
        snprintf(t->alarm_buf,     sizeof(t->alarm_buf),     "0");
        snprintf(t->alarm_dur_buf, sizeof(t->alarm_dur_buf), "10");
        snprintf(t->dilution_buf,  sizeof(t->dilution_buf),  "Stock");
        snprintf(t->iso_used_buf,  sizeof(t->iso_used_buf),  "400");
    }
}

long
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void
AdjustBuf(char *buf, size_t bufsz, int delta, int maxval)
{
    long val = strtol(buf, NULL, 10) + delta;
    if (val < 0)      val = maxval;
    if (val > maxval) val = 0;
    snprintf(buf, bufsz, "%02ld", val);
}

void
FireAlarmPulse(CountdownTimer *t)
{
    beep();
    t->alarm_pulses_left--;
}

static int
GetAlarmDurationPulses(CountdownTimer *t)
{
    long secs = strtol(t->alarm_dur_buf, NULL, 10);
    int p;
    if (secs < 0) secs = 0;
    p = (int)((secs * 1000) / ALARM_PULSE_MS);
    return p < 1 ? 1 : p;
}

static void
StartAlarm(CountdownTimer *t)
{
    if (!t->has_alarm) return;
    t->alarm_pulses_left = GetAlarmDurationPulses(t);
    FireAlarmPulse(t);
}

void StopAlarm(CountdownTimer *t) { t->alarm_pulses_left = 0; }

int
AnyTimerRunning(void)
{
    int i;
    for (i = 0; i < TIMER_COUNT; i++)
        if (g_timers[i].running) return 1;
    return 0;
}

void
ParseCountdownFields(CountdownTimer *t)
{
    long hh = strtol(t->hh_buf, NULL, 10);
    long mm = strtol(t->mm_buf, NULL, 10);
    long ss = strtol(t->ss_buf, NULL, 10);
    if (hh < 0) hh = 0; if (hh > 999) hh = 999;
    if (mm < 0) mm = 0; if (mm > 59)  mm = 59;
    if (ss < 0) ss = 0; if (ss > 59)  ss = 59;
    t->remaining = (int)(hh * 3600 + mm * 60 + ss);
}

void
SetTimerTime(CountdownTimer *t, int secs)
{
    snprintf(t->hh_buf, sizeof(t->hh_buf), "%02d", secs / 3600);
    snprintf(t->mm_buf, sizeof(t->mm_buf), "%02d", (secs % 3600) / 60);
    snprintf(t->ss_buf, sizeof(t->ss_buf), "%02d", secs % 60);
}

void
UpdateCountdownFields(CountdownTimer *t)
{
    SetTimerTime(t, t->remaining);
}

void
CountdownDoStart(CountdownTimer *t)
{
    if (t->running) return;
    ParseCountdownFields(t);
    if (t->remaining <= 0) {
        snprintf(g_status, sizeof(g_status), "%s: set time > 0 first.", t->label);
        return;
    }
    t->total   = t->remaining;
    t->running = 1;
    t->next_tick_ms = now_ms() + TICK_MS;
    snprintf(g_status, sizeof(g_status), "%s: started.", t->label);
}

void
CountdownDoStop(CountdownTimer *t)
{
    t->running = 0;
    StopAlarm(t);
    snprintf(g_status, sizeof(g_status), "%s: stopped.", t->label);
}

void
CountdownDoReset(CountdownTimer *t)
{
    CountdownDoStop(t);
    t->remaining = 0;
    t->total     = 0;
    UpdateCountdownFields(t);
    snprintf(g_status, sizeof(g_status), "%s: reset.", t->label);
}

void
CountdownTick(CountdownTimer *t)
{
    int interval;
    t->remaining--;
    UpdateCountdownFields(t);
    if (t->remaining <= 0) {
        t->running = 0;
        StartAlarm(t);
        snprintf(g_status, sizeof(g_status), "*** %s: DONE! ***", t->label);
        if      (t == &g_timers[0] && g_workflow_phase == 1) g_workflow_phase = 2;
        else if (t == &g_timers[1] && g_workflow_phase == 3) g_workflow_phase = 4;
        else if (t == &g_timers[2] && g_workflow_phase == 5) g_workflow_phase = 6;
        return;
    }
    if (!t->has_alarm) return;
    interval = (int)strtol(t->alarm_buf, NULL, 10);
    if (interval > 0 && t->remaining % interval == 0) StartAlarm(t);
}
