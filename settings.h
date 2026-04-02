#pragma once
#include <windows.h>
#include "hw_detect.h"

#define SETTINGS_COM_NAME_LEN 16

/* Main window (local_neural_monitoring.c) must handle these for Settings Auto-detect. */
#ifndef WM_APP_SETTINGS_PAUSE_SERIAL
#define WM_APP_SETTINGS_PAUSE_SERIAL  (WM_APP + 54)
#define WM_APP_SETTINGS_RESUME_SERIAL (WM_APP + 55)
#endif

typedef struct SettingsDetectResult {
    int found;
    HardwareKind hk;
    char path[64];
} SettingsDetectResult;

void settings_ini_path(char* buf, size_t buf_len);

/* Single saved COM name (e.g. COM3). Migrates legacy INI keys on first load. */
void settings_load(char* port_out, size_t name_len);

void settings_save(const char* port);

int settings_enum_ports(char (*names)[SETTINGS_COM_NAME_LEN], int max_names);

int settings_port_in_enum(const char* com_name);

void settings_auto_pick(char* out, size_t out_len);

void com_name_to_device_path(const char* com_name, char* path_out, size_t path_len);

void settings_resolve_port(char* port_out, size_t port_len, int cmdline_overrode_port);

/* Returns IDOK if user saved, IDCANCEL if dismissed.
 * prefer_com_name: if non-NULL (e.g. "COM5"), selects that port when listed; use current app port while connected. */
INT_PTR settings_show_dialog(HWND owner, HINSTANCE inst, const char* prefer_com_name);
