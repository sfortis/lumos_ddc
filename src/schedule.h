#ifndef SCHEDULE_H
#define SCHEDULE_H

#define MAX_SCHEDULE 24

/* A schedule anchor: brightness at a minute of the local day.
   Pure/Win32-free so it can be unit-tested with native gcc. */
typedef struct {
    int minutes;     /* 0-1439, minutes since local midnight */
    int brightness;  /* 0-100 */
} SchedulePoint;

/* Interpolated brightness (0-100) for minuteOfDay. Assumes pts sorted ascending
   by minutes. count==0 -> returns 0 (caller treats schedule as inactive);
   count==1 -> constant; otherwise piecewise linear with 24h wrap. */
int Schedule_BrightnessAt(const SchedulePoint *pts, int count, int minuteOfDay);

/* Minute of the next anchor strictly after minuteOfDay, wrapping past midnight.
   count==0 -> -1. */
int Schedule_NextAnchorMinute(const SchedulePoint *pts, int count, int minuteOfDay);

/* On a 24h circle, has nowMinute reached/passed resumeMinute since suspendMinute?
   Returns 1 to resume, 0 to stay suspended. */
int Schedule_ShouldResume(int suspendMinute, int resumeMinute, int nowMinute);

/* Parse "HH:MM" -> minutes 0..1439, or -1 if malformed/out of range. */
int Schedule_ParseKeyTime(const char *hhmm);

/* Insertion-sort ascending by minutes (count <= MAX_SCHEDULE). */
void Schedule_Sort(SchedulePoint *pts, int count);

#endif /* SCHEDULE_H */
