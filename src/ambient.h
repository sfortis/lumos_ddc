#ifndef AMBIENT_H
#define AMBIENT_H

/* Auto brightness from an illuminance sensor: the curve that turns lux into
 * an All Monitors level, how the curve learns from manual changes, and the
 * smoothing that keeps a noisy sensor from making the screens flicker.
 *
 * The curve is a list of (lux, level) points sorted by lux. Between points
 * the level is interpolated over log10(lux + AMBIENT_LUX_OFFSET), because the
 * eye judges light on a ratio scale: 50 to 100 lx is as large a step as 500
 * to 1000 lx. The offset flattens the scale below about 10 lx, where 0 and
 * 5 lx are both a dark room; with an offset of 1 a noisy sensor that flips
 * between 0 and 5 lx looked like a large change. Below the first point and
 * above the last one the level stays flat.
 *
 * Win32-free, unit-tested in test_ambient.c. */

#define AMBIENT_MAX_POINTS 8

#define AMBIENT_LUX_OFFSET 10.0

/* Two points closer than this on the log scale are the same light level, so
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
 * reports 0, 5 or 8 lx at random while the room is dark. Over 16 hours of
 * 15 s polls the level changes about 23 times for 0/5 noise and 65 times
 * for 0/5/8 noise, in steps smaller than AMBIENT_BIG. A lamp switched on (8 to 77 lx) is applied after 11 s on
 * average and 19 s at worst, and a step from 77 to 130 lx within 45 s.
 * A threshold of 0.35 took up to 210 s for that step, because it went
 * through the slow path; below 0.2 the noise itself starts to take the
 * confirmed path (0.17 gave 660 changes in 16 h).
 *
 * A large change must also be AMBIENT_MIN_JUMP_LUX in lux, so the 0 to 8 lx
 * noise of a dark room always takes the slow path and moves the level in
 * small steps instead of a jump. The confirming reading must be close to the
 * held one, so a spike after noise is not confirmed by it; a reading caught
 * halfway through a lamp's ramp is held again and confirmed 3 s later.
 *
 * A reading less than AMBIENT_STEP from the smoothed value (on the log scale)
 * is noise or a slow drift and moves it by AMBIENT_SLOW of the distance. A
 * larger one is held until the next reading confirms it, on the same side
 * and close to it; then the value jumps to the pair. A real change persists
 * and a noise spike does not, which tells them apart where their sizes are
 * alike. */
#define AMBIENT_STEP 0.2
#define AMBIENT_MIN_JUMP_LUX 15.0
#define AMBIENT_SLOW 0.08

typedef struct {
    int    primed;      /* a first reading has been taken */
    double logLux;      /* smoothed value on the log scale */
    int    pending;     /* a large deviation waits for confirmation */
    double pendingLog;  /* that reading */
} AmbientFilter;

/* Feed one reading (lux >= 0) and return the smoothed lux. */
double Ambient_Smooth(AmbientFilter *f, double lux);

/* The light the room is in now as far as the filter knows: a large change
   still waiting for confirmation, otherwise the smoothed value. */
double Ambient_LatestLux(const AmbientFilter *f);

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
