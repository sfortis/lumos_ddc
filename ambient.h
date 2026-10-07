#ifndef AMBIENT_H
#define AMBIENT_H

/* Auto brightness from an illuminance sensor: the curve that turns lux into
 * an All Monitors level, how the curve learns from manual changes, and the
 * smoothing that keeps a noisy sensor from making the screens flicker.
 *
 * The curve is a list of (lux, level) points sorted by lux. Between points
 * the level is interpolated over log10(lux + 1), because the eye judges
 * light on a ratio scale: 5 to 10 lx is as large a step as 500 to 1000 lx.
 * Below the first point and above the last one the level stays flat.
 *
 * Win32-free, unit-tested in test_ambient.c. */

#define AMBIENT_MAX_POINTS 8

/* Two points closer than this in log10(lux + 1) are the same light level, so
   a new point replaces the old one instead of crowding next to it. */
#define AMBIENT_SAME_LIGHT 0.15


typedef struct {
    double lux;
    int    level;   /* All Monitors level, 0-100 */
} AmbientPoint;

typedef struct {
    AmbientPoint points[AMBIENT_MAX_POINTS];
    int          count;
} AmbientCurve;

/* The curve used until anything is learned: 0 lx 20%, 10 lx 40%,
   100 lx 70%, 1000 lx 100%. */
void Ambient_DefaultCurve(AmbientCurve *c);

/* The level for this lux. An empty curve gives the default curve's level. */
int Ambient_LevelFor(const AmbientCurve *c, double lux);

/* The user set this level at this lux: make it a point of the curve. Points
   at the same light level are replaced, and points that would make the
   curve go down as the light goes up are removed. When the curve is full,
   the point nearest the new one goes. */
void Ambient_Learn(AmbientCurve *c, double lux, int level);

/* Read "lux:level,lux:level" (the config.ini form). Points out of range are
   dropped and the rest are sorted and made consistent. Returns the number of
   points, 0 for an empty or unreadable text. */
int Ambient_Parse(const char *text, AmbientCurve *c);

/* Write the curve as "lux:level,lux:level". Returns the length, or -1 when out
   is too small. */
int Ambient_Format(const AmbientCurve *c, char *out, int cap);

/* Smoothing and gating, tuned by simulating the Living Room FP2, which
 * reports 0 or 5 lx at random. With plain smoothing the level changed about
 * every 5 minutes; with the rules below about every half hour, while a lamp
 * switched on still shows within one reading.
 *
 * Smoothing is exponential in log space and adaptive. A reading less than
 * AMBIENT_JUMP decades from the smoothed value (sensor noise, passing clouds)
 * moves it by AMBIENT_SLOW of the distance; a larger one (a lamp switched on
 * or off) moves it by AMBIENT_FAST. */
#define AMBIENT_JUMP 1.0
#define AMBIENT_SLOW 0.08
#define AMBIENT_FAST 0.85

typedef struct {
    int    primed;    /* a first reading has been taken */
    double logLux;    /* smoothed log10(lux + 1) */
} AmbientFilter;

/* Feed one reading (lux >= 0) and return the smoothed lux. */
double Ambient_Smooth(AmbientFilter *f, double lux);

/* The gate decides when the target level is applied. A change of at least
   AMBIENT_BIG levels is applied at once. A smaller one must be at least
   AMBIENT_HYSTERESIS levels (or reach 0 or 100) and point the same way for
   AMBIENT_PERSIST readings in a row. */
#define AMBIENT_HYSTERESIS 3
#define AMBIENT_PERSIST    3
#define AMBIENT_BIG        15

typedef struct {
    int lastApplied;   /* -1 until the first apply */
    int direction;     /* sign of the pending change, 0 for none */
    int count;         /* readings in a row that pointed that way */
} AmbientGate;

/* Start a gate with nothing applied. */
void Ambient_GateReset(AmbientGate *g);

/* Feed one target level. Returns nonzero when it should be applied now, and
   then records it as the level last applied. */
int Ambient_Decide(AmbientGate *g, int target);

#endif /* AMBIENT_H */
