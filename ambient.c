#include "ambient.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const AmbientPoint kDefaultPoints[] = {
    { 0, 20 }, { 10, 40 }, { 100, 70 }, { 1000, 100 }
};

static double LogLux(double lux)
{
    return log10((lux < 0 ? 0 : lux) + 1.0);
}

static int ClampLevel(int v)
{
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

void Ambient_DefaultCurve(AmbientCurve *c)
{
    c->count = (int)(sizeof(kDefaultPoints) / sizeof(kDefaultPoints[0]));
    memcpy(c->points, kDefaultPoints, sizeof(kDefaultPoints));
}

int Ambient_LevelFor(const AmbientCurve *c, double lux)
{
    AmbientCurve def;
    if (c->count == 0) {
        Ambient_DefaultCurve(&def);
        c = &def;
    }
    const AmbientPoint *p = c->points;
    int n = c->count;
    if (lux <= p[0].lux)
        return p[0].level;
    if (lux >= p[n - 1].lux)
        return p[n - 1].level;
    double x = LogLux(lux);
    for (int i = 1; i < n; i++) {
        if (lux <= p[i].lux) {
            double x0 = LogLux(p[i - 1].lux), x1 = LogLux(p[i].lux);
            double t = (x1 > x0) ? (x - x0) / (x1 - x0) : 1.0;
            double v = p[i - 1].level + t * (p[i].level - p[i - 1].level);
            return ClampLevel((int)floor(v + 0.5));
        }
    }
    return p[n - 1].level;
}

static void RemoveAt(AmbientCurve *c, int i)
{
    memmove(&c->points[i], &c->points[i + 1], (size_t)(c->count - i - 1) * sizeof(AmbientPoint));
    c->count--;
}

/* Insert keeping the points sorted by lux. The caller makes room. */
static void InsertSorted(AmbientCurve *c, double lux, int level)
{
    int i = c->count;
    while (i > 0 && c->points[i - 1].lux > lux) {
        c->points[i] = c->points[i - 1];
        i--;
    }
    c->points[i].lux = lux;
    c->points[i].level = level;
    c->count++;
}

void Ambient_Learn(AmbientCurve *c, double lux, int level)
{
    if (lux < 0) lux = 0;
    level = ClampLevel(level);
    if (c->count == 0)
        Ambient_DefaultCurve(c);

    double x = LogLux(lux);
    for (int i = c->count - 1; i >= 0; i--) {
        const AmbientPoint *p = &c->points[i];
        int sameLight = fabs(LogLux(p->lux) - x) < AMBIENT_SAME_LIGHT;
        int contradicts = (p->lux < lux && p->level > level) || (p->lux > lux && p->level < level);
        if (sameLight || contradicts)
            RemoveAt(c, i);
    }
    if (c->count >= AMBIENT_MAX_POINTS) {
        int nearest = 0;
        double best = 1e9;
        for (int i = 0; i < c->count; i++) {
            double d = fabs(LogLux(c->points[i].lux) - x);
            if (d < best) { best = d; nearest = i; }
        }
        RemoveAt(c, nearest);
    }
    InsertSorted(c, lux, level);
}

int Ambient_Parse(const char *text, AmbientCurve *c)
{
    c->count = 0;
    const char *s = text;
    while (s && *s) {
        char *end;
        double lux = strtod(s, &end);
        if (end == s || *end != ':')
            break;
        s = end + 1;
        long level = strtol(s, &end, 10);
        if (end == s)
            break;
        s = end;
        /* Learn applies the same rules a manual change would, so a hand-edited
           line that contradicts itself still ends up as a rising curve. */
        if (lux >= 0 && lux <= 1e6 && level >= 0 && level <= 100) {
            if (c->count == 0) {
                c->points[0].lux = lux;
                c->points[0].level = (int)level;
                c->count = 1;
            } else {
                Ambient_Learn(c, lux, (int)level);
            }
        }
        if (*s == ',')
            s++;
        else
            break;
    }
    return c->count;
}

int Ambient_Format(const AmbientCurve *c, char *out, int cap)
{
    int len = 0;
    if (cap <= 0) return -1;
    out[0] = '\0';
    for (int i = 0; i < c->count; i++) {
        int n = snprintf(out + len, (size_t)(cap - len), "%s%g:%d",
                         i ? "," : "", c->points[i].lux, c->points[i].level);
        if (n < 0 || n >= cap - len)
            return -1;
        len += n;
    }
    return len;
}

double Ambient_Smooth(AmbientFilter *f, double lux)
{
    double x = LogLux(lux);
    if (!f->primed) {
        f->primed = 1;
        f->logLux = x;
    } else {
        double d = x - f->logLux;
        f->logLux += (fabs(d) >= AMBIENT_JUMP ? AMBIENT_FAST : AMBIENT_SLOW) * d;
    }
    return pow(10.0, f->logLux) - 1.0;
}

void Ambient_GateReset(AmbientGate *g)
{
    g->lastApplied = -1;
    g->direction = 0;
    g->count = 0;
}

static int Accept(AmbientGate *g, int target)
{
    g->lastApplied = target;
    g->direction = 0;
    g->count = 0;
    return 1;
}

int Ambient_Decide(AmbientGate *g, int target)
{
    if (g->lastApplied < 0)
        return Accept(g, target);
    int diff = target - g->lastApplied;
    int dist = abs(diff);
    if (dist >= AMBIENT_BIG)
        return Accept(g, target);
    /* The ends count at any distance, or 100 would never be reached from 98. */
    int enough = dist >= AMBIENT_HYSTERESIS || (dist > 0 && (target == 0 || target == 100));
    if (!enough) {
        g->direction = 0;
        g->count = 0;
        return 0;
    }
    int dir = diff > 0 ? 1 : -1;
    if (dir == g->direction) {
        g->count++;
    } else {
        g->direction = dir;
        g->count = 1;
    }
    return g->count >= AMBIENT_PERSIST ? Accept(g, target) : 0;
}
