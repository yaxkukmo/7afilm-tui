#ifndef TIMER_H
#define TIMER_H

#include <stddef.h> /* size_t */

#define TICK_MS        1000
#define ALARM_PULSE_MS  500
#define TIMER_COUNT       3

typedef struct {
    const char *label;
    int has_alarm;
    int has_temp;
    int has_recipe;
    char hh_buf[8];
    char mm_buf[8];
    char ss_buf[8];
    char alarm_buf[8];
    char alarm_dur_buf[8];
    char dev_name_buf[32];
    char dilution_buf[24];
    char film_buf[32];
    char iso_buf[8];
    char iso_used_buf[8];
    int  remaining;
    int  total;
    int  running;
    long next_tick_ms;
    int  alarm_pulses_left;
    long next_alarm_pulse_ms;
} CountdownTimer;

extern CountdownTimer g_timers[TIMER_COUNT];

/*
 * These globals live in 7afilm-tui.c but timer logic writes to them.
 * Declared here so timer.c can reference them without a full globals header.
 */
extern char g_status[128];
extern int  g_workflow_phase;

void InitTimers(void);
void FireAlarmPulse(CountdownTimer *t);
long now_ms(void);
void AdjustBuf(char *buf, size_t bufsz, int delta, int maxval);
int  AnyTimerRunning(void);
void ParseCountdownFields(CountdownTimer *t);
void UpdateCountdownFields(CountdownTimer *t);
void SetTimerTime(CountdownTimer *t, int secs);
void CountdownDoStart(CountdownTimer *t);
void CountdownDoStop(CountdownTimer *t);
void CountdownDoReset(CountdownTimer *t);
void CountdownTick(CountdownTimer *t);

#endif /* TIMER_H */
