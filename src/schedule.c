#include "schedule.h"

/* Clockwise distance a->b on a 1440-minute circle. */
static int fwd(int a, int b)
{
    int d = b - a;
    if (d < 0) d += 1440;
    return d;
}

static int lerp_round(int a, int b, int pos, int span)
{
    if (span <= 0) return a;
    /* round to nearest */
    return a + ((b - a) * pos + (span / 2) * ((b - a) >= 0 ? 1 : -1)) / span;
}

int Schedule_BrightnessAt(const SchedulePoint *pts, int count, int minuteOfDay)
{
    if (count <= 0) return 0;
    if (count == 1) return pts[0].brightness;

    /* Inside the day between two consecutive anchors */
    if (minuteOfDay >= pts[0].minutes && minuteOfDay < pts[count - 1].minutes) {
        for (int i = 0; i < count - 1; i++) {
            if (minuteOfDay >= pts[i].minutes && minuteOfDay < pts[i + 1].minutes) {
                int span = pts[i + 1].minutes - pts[i].minutes;
                int pos = minuteOfDay - pts[i].minutes;
                return lerp_round(pts[i].brightness, pts[i + 1].brightness, pos, span);
            }
        }
    }

    /* Wrap segment: last anchor -> first anchor across midnight */
    int span = fwd(pts[count - 1].minutes, pts[0].minutes);
    int pos = fwd(pts[count - 1].minutes, minuteOfDay);
    return lerp_round(pts[count - 1].brightness, pts[0].brightness, pos, span);
}

int Schedule_NextAnchorMinute(const SchedulePoint *pts, int count, int minuteOfDay)
{
    if (count <= 0) return -1;
    for (int i = 0; i < count; i++)
        if (pts[i].minutes > minuteOfDay)
            return pts[i].minutes;
    return pts[0].minutes;  /* wrap to tomorrow's first anchor */
}

int Schedule_ShouldResume(int suspendMinute, int resumeMinute, int nowMinute)
{
    int dr = fwd(suspendMinute, resumeMinute);  /* arc suspend -> resume */
    int dn = fwd(suspendMinute, nowMinute);     /* arc suspend -> now */
    if (dr == 0) return 1;
    return dn >= dr ? 1 : 0;
}

int Schedule_ParseKeyTime(const char *hhmm)
{
    if (!hhmm) return -1;
    /* Expect exactly HH:MM (2 digits, colon, 2 digits) */
    if (hhmm[0] < '0' || hhmm[0] > '9') return -1;
    if (hhmm[1] < '0' || hhmm[1] > '9') return -1;
    if (hhmm[2] != ':') return -1;
    if (hhmm[3] < '0' || hhmm[3] > '9') return -1;
    if (hhmm[4] < '0' || hhmm[4] > '9') return -1;
    if (hhmm[5] != '\0') return -1;
    int h = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    int m = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

void Schedule_Sort(SchedulePoint *pts, int count)
{
    for (int i = 1; i < count; i++) {
        SchedulePoint key = pts[i];
        int j = i - 1;
        while (j >= 0 && pts[j].minutes > key.minutes) {
            pts[j + 1] = pts[j];
            j--;
        }
        pts[j + 1] = key;
    }
}
