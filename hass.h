#ifndef HASS_H
#define HASS_H

#include <windows.h>

/* Home Assistant client over WinHTTP.
 *
 * Every call here blocks for up to a few seconds on the network, so callers
 * run it on a worker thread, never on the UI thread (the mouse hook lives
 * there too). The token is a long-lived access token; a non-admin user's
 * token is enough for both calls:
 * - Hass_ReadLux reads one sensor with GET /api/states/<entity_id>.
 * - Hass_ListSensors lists the illuminance sensors with their area through
 *   the WebSocket API (render_template). The REST template endpoint would be
 *   simpler, but it requires an admin user.
 * Certificate validation is never turned off; a TLS failure is reported. */

#define HASS_URL_MAX     256
#define HASS_TOKEN_MAX   512
#define HASS_ENTITY_MAX  128
#define HASS_MAX_SENSORS 64

typedef enum {
    HASS_OK = 0,
    HASS_ERR_URL,        /* the URL cannot be parsed */
    HASS_ERR_RESOLVE,    /* host name not found */
    HASS_ERR_CONNECT,    /* no answer: wrong address, HA down, or no VPN */
    HASS_ERR_TLS,        /* the certificate was not accepted */
    HASS_ERR_AUTH,       /* the token was refused */
    HASS_ERR_NOT_FOUND,  /* no such entity */
    HASS_ERR_HTTP,       /* another HTTP status */
    HASS_ERR_PROTOCOL    /* a reply Lumos did not understand */
} HassStatus;

typedef struct {
    WCHAR  entityId[HASS_ENTITY_MAX];
    WCHAR  name[128];
    WCHAR  area[64];     /* empty when neither the entity nor its device has one */
    double lux;
    BOOL   hasValue;     /* FALSE for "unavailable", "unknown" or text */
} HassSensor;

/* Read the current value of one sensor. On HASS_OK, *hasValue says whether
   the state was a number; *lux is set only then. */
HassStatus Hass_ReadLux(const WCHAR *url, const char *token, const WCHAR *entityId,
                        double *lux, BOOL *hasValue);

/* List the sensors whose device_class is illuminance, at most max of them. */
HassStatus Hass_ListSensors(const WCHAR *url, const char *token,
                            HassSensor *out, int max, int *count);

/* A short sentence for the user, such as "The token was refused." */
const WCHAR *Hass_StatusText(HassStatus status);

#endif /* HASS_H */
