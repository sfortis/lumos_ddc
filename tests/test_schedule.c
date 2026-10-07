#include "schedule.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    SchedulePoint pts[MAX_SCHEDULE];

    /* Single point -> constant all day */
    pts[0].minutes = 8 * 60; pts[0].brightness = 70;
    CHECK(Schedule_BrightnessAt(pts, 1, 0) == 70, "single point midnight");
    CHECK(Schedule_BrightnessAt(pts, 1, 23 * 60) == 70, "single point evening");

    /* Two points: 08:00=80, 20:00=20 */
    pts[0].minutes = 8 * 60;  pts[0].brightness = 80;
    pts[1].minutes = 20 * 60; pts[1].brightness = 20;
    CHECK(Schedule_BrightnessAt(pts, 2, 8 * 60) == 80, "endpoint start");
    CHECK(Schedule_BrightnessAt(pts, 2, 20 * 60) == 20, "endpoint end");
    /* Midpoint 14:00 -> halfway between 80 and 20 = 50 */
    CHECK(Schedule_BrightnessAt(pts, 2, 14 * 60) == 50, "midpoint interpolation");

    /* Wrap: before first point (02:00) interpolates last->first across midnight.
       From 20:00 (20) to 08:00 (80): span=720 min, 02:00 is 360 min in -> 50 */
    CHECK(Schedule_BrightnessAt(pts, 2, 2 * 60) == 50, "wrap before first");
    /* After last point 23:00 -> 180 min into the wrap of 720 -> 20 + 60*0.25 = 35 */
    CHECK(Schedule_BrightnessAt(pts, 2, 23 * 60) == 35, "wrap after last");

    /* Next anchor */
    CHECK(Schedule_NextAnchorMinute(pts, 2, 10 * 60) == 20 * 60, "next anchor same day");
    CHECK(Schedule_NextAnchorMinute(pts, 2, 22 * 60) == 8 * 60, "next anchor wraps");

    /* ShouldResume on the circle. Suspend at 14:00, resume anchor 20:00. */
    CHECK(Schedule_ShouldResume(14 * 60, 20 * 60, 14 * 60) == 0, "not resumed at suspend");
    CHECK(Schedule_ShouldResume(14 * 60, 20 * 60, 19 * 60) == 0, "not resumed before anchor");
    CHECK(Schedule_ShouldResume(14 * 60, 20 * 60, 20 * 60) == 1, "resumed at anchor");
    /* Wrap case: suspend 22:00, resume anchor 08:00 next day */
    CHECK(Schedule_ShouldResume(22 * 60, 8 * 60, 23 * 60) == 0, "wrap not resumed");
    CHECK(Schedule_ShouldResume(22 * 60, 8 * 60, 8 * 60) == 1, "wrap resumed");

    /* Parse */
    CHECK(Schedule_ParseKeyTime("08:00") == 480, "parse 08:00");
    CHECK(Schedule_ParseKeyTime("23:30") == 1410, "parse 23:30");
    CHECK(Schedule_ParseKeyTime("24:00") == -1, "reject 24:00");
    CHECK(Schedule_ParseKeyTime("8-00") == -1, "reject bad separator");
    CHECK(Schedule_ParseKeyTime("08:99") == -1, "reject bad minute");

    /* Sort */
    pts[0].minutes = 1200; pts[0].brightness = 1;
    pts[1].minutes = 300;  pts[1].brightness = 2;
    pts[2].minutes = 800;  pts[2].brightness = 3;
    Schedule_Sort(pts, 3);
    CHECK(pts[0].minutes == 300 && pts[1].minutes == 800 && pts[2].minutes == 1200, "sort ascending");

    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
