#include "ambient.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int Rising(const AmbientCurve *c)
{
    for (int i = 1; i < c->count; i++)
        if (c->points[i].lux <= c->points[i - 1].lux || c->points[i].level < c->points[i - 1].level)
            return 0;
    return 1;
}

int main(void)
{
    AmbientCurve c;
    char msg[128], text[256];

    /* Default curve */
    Ambient_DefaultCurve(&c);
    CHECK(Ambient_LevelFor(&c, 0) == 20 && Ambient_LevelFor(&c, 10) == 40, "default points");
    CHECK(Ambient_LevelFor(&c, 100) == 70 && Ambient_LevelFor(&c, 1000) == 100, "default points high");
    CHECK(Ambient_LevelFor(&c, 50000) == 100, "flat above the last point");
    int mid = Ambient_LevelFor(&c, 30);
    CHECK(mid > 40 && mid < 70, "between points");
    AmbientCurve empty = { .count = 0 };
    CHECK(Ambient_LevelFor(&empty, 10) == 40, "empty curve uses the default");

    /* Interpolation is over log(lux + 10): 5 lx is past the middle of 0..10 */
    int five = Ambient_LevelFor(&c, 5);
    CHECK(five > 30 && five < 40, "log interpolation at 5 lx");

    /* Monotonic over a sweep */
    int prev = -1, ok = 1;
    for (double lux = 0; lux < 30000; lux = lux * 1.1 + 0.1) {
        int v = Ambient_LevelFor(&c, lux);
        if (v < prev) ok = 0;
        prev = v;
    }
    CHECK(ok, "default curve never goes down");

    /* Learning a point makes the curve pass through it */
    Ambient_Learn(&c, 5, 30);
    CHECK(Ambient_LevelFor(&c, 5) == 30, "learned point is hit");
    CHECK(Rising(&c), "still rising after learning");
    CHECK(Ambient_LevelFor(&c, 0) == 20 && Ambient_LevelFor(&c, 100) == 70, "other points kept");

    /* The same light replaces the point instead of adding one */
    int before = c.count;
    Ambient_Learn(&c, 5.5, 35);
    CHECK(c.count == before && Ambient_LevelFor(&c, 5.5) == 35, "same light replaces");

    /* A contradicting point removes the points it contradicts */
    Ambient_DefaultCurve(&c);
    Ambient_Learn(&c, 10, 80);    /* brighter than the 100 lx point (70) */
    CHECK(Rising(&c), "contradiction resolved");
    CHECK(Ambient_LevelFor(&c, 10) == 80, "contradicting point wins");
    CHECK(Ambient_LevelFor(&c, 1000) >= 80, "higher lux not dimmer");
    Ambient_Learn(&c, 1000, 10);  /* darker than everything below */
    CHECK(Rising(&c) && Ambient_LevelFor(&c, 1000) == 10 && Ambient_LevelFor(&c, 0) <= 10,
          "dark point at high lux flattens the curve below it");

    /* Many points: the curve stays within its size and keeps rising */
    Ambient_DefaultCurve(&c);
    for (int i = 0; i < 40; i++) {
        double lux = (i * 37) % 2000;
        int level = 10 + (int)(lux / 25);
        Ambient_Learn(&c, lux, level > 100 ? 100 : level);
        snprintf(msg, sizeof msg, "rising after learn %d", i);
        CHECK(Rising(&c) && c.count <= AMBIENT_MAX_POINTS, msg);
    }
    CHECK(Ambient_LevelFor(&c, (39 * 37) % 2000) == 10 + (39 * 37 % 2000) / 25 ||
          Ambient_LevelFor(&c, (39 * 37) % 2000) == 100, "last learned point is hit");

    /* Clamping */
    Ambient_DefaultCurve(&c);
    Ambient_Learn(&c, -5, 150);
    CHECK(c.points[0].lux == 0 && c.points[0].level == 100, "lux and level clamped");
    Ambient_DefaultCurve(&c);
    Ambient_Learn(&c, 5e6, 90);
    CHECK(c.points[c.count - 1].lux == 1e6, "lux above a million clamped, so Parse reads it back");
    AmbientFilter inf = { 0 };
    double huge = 1e308 * 10;
    CHECK(Ambient_Smooth(&inf, huge) <= 1e6 + 1, "an infinite reading is clamped (to 1e6, give or take rounding)");

    /* The latest lux is the pending one while a change waits */
    AmbientFilter late = { 0 };
    Ambient_Smooth(&late, 8);
    Ambient_Smooth(&late, 300);
    CHECK(Ambient_LatestLux(&late) > 290 && Ambient_LatestLux(&late) < 310, "latest lux is the held reading");

    /* Parse and format */
    Ambient_DefaultCurve(&c);
    Ambient_Learn(&c, 5, 30);
    CHECK(Ambient_Format(&c, text, sizeof text) > 0, "format");
    AmbientCurve d;
    CHECK(Ambient_Parse(text, &d) == c.count, "parse count");
    ok = 1;
    for (int i = 0; i < c.count; i++)
        if (d.points[i].lux != c.points[i].lux || d.points[i].level != c.points[i].level) ok = 0;
    CHECK(ok, "parse round trip");
    CHECK(Ambient_Format(&c, text, 5) == -1, "format into a small buffer");
    CHECK(Ambient_Parse("", &d) == 0 && Ambient_Parse("garbage", &d) == 0, "empty and unreadable");
    CHECK(Ambient_Parse("100:90,10:20", &d) == 2 && d.points[0].lux == 10, "sorted on parse");
    CHECK(Ambient_Parse("10:80,100:20", &d) == 1 && Rising(&d), "self-contradicting text made rising");
    CHECK(Ambient_Parse("10:20,-1:5,20:200,30:40", &d) == 2, "out-of-range points dropped");
    CHECK(Ambient_Parse("2.5:15", &d) == 1 && d.points[0].lux == 2.5, "fractional lux");

    /* Smoothing */
    AmbientFilter f = { 0 };
    double s = Ambient_Smooth(&f, 0);
    CHECK(s == 0, "first reading taken as is");
    AmbientFilter lamp = { 0 };
    Ambient_Smooth(&lamp, 8);
    s = Ambient_Smooth(&lamp, 77);
    CHECK(s < 10, "one large reading is held");
    s = Ambient_Smooth(&lamp, 77);
    CHECK(s > 70 && s < 80, "a second one confirms it: the 8 to 77 lx lamp is followed after 30 s");
    for (int i = 0; i < 60; i++) s = Ambient_Smooth(&lamp, 77);
    CHECK(s > 75 && s < 79, "and stays there");
    AmbientFilter spike = { 0 };
    Ambient_Smooth(&spike, 8);
    Ambient_Smooth(&spike, 300);
    s = Ambient_Smooth(&spike, 8);
    CHECK(s < 10, "a single spike is ignored");
    AmbientFilter flip = { 0 };
    Ambient_Smooth(&flip, 8);
    Ambient_Smooth(&flip, 300);
    s = Ambient_Smooth(&flip, 0.5);
    CHECK(s < 10, "an opposite second reading does not confirm the first");

    /* The noisy FP2: random 0 or 5 lx for 16 hours of 30 s polls changes the
       level rarely (about 12 times in the simulation). */
    Ambient_DefaultCurve(&c);
    AmbientFilter noisy = { 0 };
    AmbientGate gate;
    Ambient_GateReset(&gate);
    unsigned seed = 7;
    int changes = 0;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        double raw = ((seed >> 16) % 3 == 0) ? 5 : 0;
        if (Ambient_Decide(&gate, Ambient_LevelFor(&c, Ambient_Smooth(&noisy, raw))) && i > 40)
            changes++;
    }
    snprintf(msg, sizeof msg, "noisy sensor changed the level %d times in 16 h", changes);
    CHECK(changes <= 20, msg);

    /* Gate */
    AmbientGate g2;
    Ambient_GateReset(&g2);
    CHECK(Ambient_Decide(&g2, 50), "first target applied");
    CHECK(!Ambient_Decide(&g2, 52), "below the hysteresis");
    CHECK(!Ambient_Decide(&g2, 54) && !Ambient_Decide(&g2, 54), "small change waits");
    CHECK(Ambient_Decide(&g2, 54) && g2.lastApplied == 54, "applied after three readings");
    CHECK(!Ambient_Decide(&g2, 58) && !Ambient_Decide(&g2, 50) && !Ambient_Decide(&g2, 58),
          "direction change restarts the count");
    CHECK(Ambient_Decide(&g2, 80) && g2.lastApplied == 80, "big change applied at once");
    Ambient_GateReset(&g2);
    Ambient_Decide(&g2, 98);
    CHECK(!Ambient_Decide(&g2, 100) && !Ambient_Decide(&g2, 100) && Ambient_Decide(&g2, 100),
          "the end is reached from 98");
    CHECK(!Ambient_Decide(&g2, 100), "no change, nothing to apply");

    if (failures == 0)
        printf("All ambient tests passed.\n");
    return failures ? 1 : 0;
}
