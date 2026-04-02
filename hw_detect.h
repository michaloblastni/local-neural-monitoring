#pragma once
#include <stddef.h>
#include <windows.h>

typedef enum {
    HW_NONE_DETECTED = 0,
    HW_EEG_SMT = 1,
    HW_OPENBCI = 2
} HardwareKind;

/* Strip \\.\ prefix for display (e.g. COM5). */
const char* hw_detect_port_display_name(const char* device_path);

/* Probe a single port: OpenBCI (115200 + 'v') then EEG-SMT (57600 + sync). Returns 1 if identified. */
int hw_detect_probe_port(const char* device_path, HardwareKind* out_kind);

/* Same as probe_port but short timeouts for UI-thread reconnect (Settings OK). */
int hw_detect_probe_port_reconnect(const char* device_path, HardwareKind* out_kind);

/* Open COM with same bounded CreateFile as detection (readwrite: 1=OpenBCI RW, 0=EEG read-only). */
HANDLE hw_detect_open_com_app(const char* device_path, int readwrite);

/* Enumerate COM ports; first OpenBCI match wins, else first EEG-SMT. Returns 1 if found. */
int hw_detect_scan(char* port_device_path, size_t path_len, HardwareKind* out_kind);

/* Resolve likely Cyton COM port from Windows (friendly name / USB instance id) before slow banner scan.
 * If trust_without_banner_out is non-NULL, set *trust_without_banner_out=1 when banner verify can be skipped
 * (OpenBCI in device name, or common USB-UART VID/PID match). */
int hw_detect_com_openbci_from_system(char* port_device_path, size_t path_len, int* trust_without_banner_out);

/* Quick Cyton banner check (same budget as per-port scan). */
int hw_detect_try_openbci_quick(const char* device_path);
