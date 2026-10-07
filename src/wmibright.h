#ifndef WMIBRIGHT_H
#define WMIBRIGHT_H

#include <windows.h>

/*
 * WMI backlight backend for internal laptop panels.
 *
 * Internal panels do not respond to DDC/CI (I2C). Windows exposes their
 * backlight through the root\WMI classes WmiMonitorBrightness (read) and
 * WmiMonitorBrightnessMethods (WmiSetBrightness). These classes are only
 * populated for integrated panels, so their presence is itself the signal
 * that a display is an internal, WMI-controlled panel.
 *
 * Not device specific: matching is done purely by the WMI-reported
 * InstanceName, never by hardcoded vendor/product IDs.
 */

#define WMI_MAX_INSTANCE 256

typedef struct {
    WCHAR instanceName[WMI_MAX_INSTANCE]; /* e.g. DISPLAY\LEN40BA\5&..._0 */
    DWORD currentBrightness;              /* 0-100 as reported by WMI */
} WmiPanel;

/* Query all WMI-controllable panels. Returns count (>=0), fills up to `max`.
 * Safe to call repeatedly; each call does its own COM init/teardown. */
int Wmi_QueryPanels(WmiPanel *out, int max);

/* Set brightness (0-100) for the panel identified by instanceName.
 * Returns TRUE on success. */
BOOL Wmi_SetBrightness(const WCHAR *instanceName, DWORD percent);

/* Read current brightness (0-100) for the panel; returns TRUE and fills
 * *outPercent on success. */
BOOL Wmi_GetBrightness(const WCHAR *instanceName, DWORD *outPercent);

#endif /* WMIBRIGHT_H */
