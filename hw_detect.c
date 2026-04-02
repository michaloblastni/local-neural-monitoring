#define _CRT_SECURE_NO_WARNINGS
#include "hw_detect.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

static int configure_serial(HANDLE h, DWORD baud) {
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) return 0;
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    return SetCommState(h, &dcb) ? 1 : 0;
}

static void set_read_timeouts(HANDLE h, DWORD read_total_ms) {
    COMMTIMEOUTS t = { 0 };
    t.ReadIntervalTimeout = 40;
    t.ReadTotalTimeoutMultiplier = 0;
    t.ReadTotalTimeoutConstant = read_total_ms;
    SetCommTimeouts(h, &t);
}

static int buffer_has_openbci_banner(const char* buf) {
    if (strstr(buf, "OpenBCI") != NULL) return 1;
    if (strstr(buf, "openbci") != NULL) return 1;
    return 0;
}

typedef struct {
    HANDLE h;
    HANDLE done_evt;
    DWORD budget_ms;
} ObciIoWatch;

/* Unblock ReadFile if a buggy driver ignores COM timeouts (CancelIoEx after budget). */
static DWORD WINAPI openbci_io_watchdog(LPVOID p) {
    ObciIoWatch* w = (ObciIoWatch*)p;
    if (WaitForSingleObject(w->done_evt, w->budget_ms) == WAIT_TIMEOUT)
        CancelIoEx(w->h, NULL);
    return 0;
}

/* Cyton: soft reset then read ASCII banner (see OpenBCI Cyton SDK). */
static int try_openbci_on_handle_ms(HANDLE h, unsigned int max_wait_ms) {
    char buf[4096];
    size_t total = 0;
    DWORD n;
    unsigned long t0;
    int ok = 0;
    ObciIoWatch watch;
    HANDLE watch_th = NULL;

    if (!configure_serial(h, CBR_115200)) return 0;
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    set_read_timeouts(h, 120);

    watch.h = h;
    watch.budget_ms = max_wait_ms + 400u;
    watch.done_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (watch.done_evt) {
        watch_th = CreateThread(NULL, 0, openbci_io_watchdog, &watch, 0, NULL);
    }

    (void)WriteFile(h, "v", 1, &n, NULL);

    t0 = GetTickCount();
    while (GetTickCount() - t0 < max_wait_ms && total < sizeof(buf) - 2) {
        if (ReadFile(h, buf + total, (DWORD)(sizeof(buf) - 1 - total), &n, NULL) && n > 0) {
            total += n;
            buf[total] = 0;
            if (buffer_has_openbci_banner(buf)) {
                ok = 1;
                goto obci_done;
            }
        } else {
            buf[total] = 0;
            if (total > 40 && buffer_has_openbci_banner(buf)) {
                ok = 1;
                goto obci_done;
            }
            Sleep(15);
        }
    }
    buf[total] = 0;
    ok = buffer_has_openbci_banner(buf) ? 1 : 0;
obci_done:
    if (watch.done_evt) {
        SetEvent(watch.done_evt);
        if (watch_th) {
            WaitForSingleObject(watch_th, 8000);
            CloseHandle(watch_th);
        }
        CloseHandle(watch.done_evt);
    }
    return ok;
}

/* Single-port probe: allow several seconds for cold USB / firmware. */
#define OPENBCI_PROBE_MS 4500u
/* Full COM enumeration: long enough for Cyton banner; still bounded per port. */
#define OPENBCI_SCAN_PORT_MS 1800u
/* Some Bluetooth/virtual COM drivers block CreateFile indefinitely — never wait longer than this. */
#define CREATEFILE_COM_TIMEOUT_MS 2500u

typedef struct {
    char path[128];
    int readwrite; /* 1 = GENERIC_READ|WRITE (OpenBCI), 0 = read-only (EEG-SMT) */
    volatile HANDLE result;
} ComOpenWork;

static DWORD WINAPI ComOpenThread(LPVOID p) {
    ComOpenWork* w = (ComOpenWork*)p;
    if (w->readwrite)
        w->result = CreateFileA(w->path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    else
        w->result = CreateFileA(w->path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    return 0;
}

static HANDLE open_com_port_deadline(const char* device_path, int readwrite, DWORD deadline_ms) {
    ComOpenWork w;
    HANDLE th;
    DWORD wr;

    if (!device_path || !device_path[0]) return INVALID_HANDLE_VALUE;
    strncpy(w.path, device_path, sizeof(w.path) - 1);
    w.path[sizeof(w.path) - 1] = 0;
    w.readwrite = readwrite ? 1 : 0;
    w.result = INVALID_HANDLE_VALUE;

    th = CreateThread(NULL, 0, ComOpenThread, &w, 0, NULL);
    if (!th) return INVALID_HANDLE_VALUE;
    wr = WaitForSingleObject(th, deadline_ms);
    if (wr == WAIT_OBJECT_0) {
        CloseHandle(th);
        return w.result;
    }
    TerminateThread(th, 0);
    CloseHandle(th);
    return INVALID_HANDLE_VALUE;
}

static int hw_detect_try_openbci_ms(const char* device_path, unsigned int max_wait_ms) {
    HANDLE h = open_com_port_deadline(device_path, 1, CREATEFILE_COM_TIMEOUT_MS);
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = try_openbci_on_handle_ms(h, max_wait_ms);
    CloseHandle(h);
    return ok;
}

static int scan_buffer_a5_5a(const unsigned char* p, size_t len) {
    size_t i;
    for (i = 0; i + 1 < len; i++) {
        if (p[i] == 0xA5 && p[i + 1] == 0x5A) return 1;
    }
    return 0;
}

#define EEG_SMT_PROBE_MS 1800u
#define EEG_SMT_SCAN_PORT_MS 900u

/* Full-machine scans: never probe more than this many COM entries or past this wall time. */
#define HW_DETECT_SCAN_MAX_PORTS 24
#define HW_DETECT_SCAN_MAX_MS 40000u

typedef struct {
    HANDLE h;
    HANDLE done_evt;
    DWORD budget_ms;
} EegIoWatch;

static DWORD WINAPI eeg_io_watchdog(LPVOID p) {
    EegIoWatch* w = (EegIoWatch*)p;
    if (WaitForSingleObject(w->done_evt, w->budget_ms) == WAIT_TIMEOUT)
        CancelIoEx(w->h, NULL);
    return 0;
}

/* OpenEEG P2-style sync (Olimex EEG-SMT). */
static int try_eeg_smt_on_handle_ms(HANDLE h, unsigned int max_wait_ms) {
    unsigned char buf[4096];
    size_t total = 0;
    DWORD n;
    unsigned long t0;
    int ok = 0;
    EegIoWatch watch;
    HANDLE watch_th = NULL;

    if (!configure_serial(h, CBR_57600)) return 0;
    PurgeComm(h, PURGE_RXCLEAR);
    set_read_timeouts(h, 100);

    watch.h = h;
    watch.budget_ms = max_wait_ms + 400u;
    watch.done_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (watch.done_evt)
        watch_th = CreateThread(NULL, 0, eeg_io_watchdog, &watch, 0, NULL);

    t0 = GetTickCount();
    while (GetTickCount() - t0 < max_wait_ms && total < sizeof(buf)) {
        if (ReadFile(h, buf + total, (DWORD)(sizeof(buf) - total), &n, NULL) && n > 0) {
            total += n;
            if (scan_buffer_a5_5a(buf, total)) {
                ok = 1;
                goto eeg_done;
            }
        } else {
            if (total >= 2 && scan_buffer_a5_5a(buf, total)) {
                ok = 1;
                goto eeg_done;
            }
            Sleep(10);
        }
    }
    ok = scan_buffer_a5_5a(buf, total) ? 1 : 0;
eeg_done:
    if (watch.done_evt) {
        SetEvent(watch.done_evt);
        if (watch_th) {
            WaitForSingleObject(watch_th, 8000);
            CloseHandle(watch_th);
        }
        CloseHandle(watch.done_evt);
    }
    return ok;
}

static int hw_detect_try_eeg_smt_ms(const char* device_path, unsigned int max_wait_ms) {
    HANDLE h = open_com_port_deadline(device_path, 0, CREATEFILE_COM_TIMEOUT_MS);
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = try_eeg_smt_on_handle_ms(h, max_wait_ms);
    CloseHandle(h);
    return ok;
}

int hw_detect_try_openbci_quick(const char* device_path) {
    if (!device_path || !device_path[0]) return 0;
    return hw_detect_try_openbci_ms(device_path, OPENBCI_SCAN_PORT_MS);
}

const char* hw_detect_port_display_name(const char* device_path) {
    if (device_path && _strnicmp(device_path, "\\\\.\\", 4) == 0)
        return device_path + 4;
    return device_path ? device_path : "?";
}

int hw_detect_probe_port(const char* device_path, HardwareKind* out_kind) {
    if (!device_path || !device_path[0] || !out_kind) return 0;

    if (hw_detect_try_openbci_ms(device_path, OPENBCI_PROBE_MS)) {
        *out_kind = HW_OPENBCI;
        return 1;
    }
    if (hw_detect_try_eeg_smt_ms(device_path, EEG_SMT_PROBE_MS)) {
        *out_kind = HW_EEG_SMT;
        return 1;
    }
    *out_kind = HW_NONE_DETECTED;
    return 0;
}

#define OPENBCI_RECONNECT_PROBE_MS 900u
#define EEG_SMT_RECONNECT_PROBE_MS 800u

HANDLE hw_detect_open_com_app(const char* device_path, int readwrite) {
    if (!device_path || !device_path[0]) return INVALID_HANDLE_VALUE;
    return open_com_port_deadline(device_path, readwrite ? 1 : 0, CREATEFILE_COM_TIMEOUT_MS);
}

int hw_detect_probe_port_reconnect(const char* device_path, HardwareKind* out_kind) {
    if (!device_path || !device_path[0] || !out_kind) return 0;

    if (hw_detect_try_openbci_ms(device_path, OPENBCI_RECONNECT_PROBE_MS)) {
        *out_kind = HW_OPENBCI;
        return 1;
    }
    if (hw_detect_try_eeg_smt_ms(device_path, EEG_SMT_RECONNECT_PROBE_MS)) {
        *out_kind = HW_EEG_SMT;
        return 1;
    }
    *out_kind = HW_NONE_DETECTED;
    return 0;
}

int hw_detect_scan(char* port_device_path, size_t path_len, HardwareKind* out_kind) {
    char list[64][SETTINGS_COM_NAME_LEN];
    int n;
    int i;
    int nlim;
    unsigned long t0;

    if (!port_device_path || path_len < 8 || !out_kind) return 0;

    *out_kind = HW_NONE_DETECTED;
    port_device_path[0] = 0;

    n = settings_enum_ports(list, 64);
    nlim = n;
    if (nlim > HW_DETECT_SCAN_MAX_PORTS) nlim = HW_DETECT_SCAN_MAX_PORTS;

    t0 = GetTickCount();
    for (i = 0; i < nlim; i++) {
        char path[64];
        if (GetTickCount() - t0 > HW_DETECT_SCAN_MAX_MS) break;

        snprintf(path, sizeof(path), "\\\\.\\%s", list[i]);
        if (hw_detect_try_openbci_ms(path, OPENBCI_SCAN_PORT_MS)) {
            strncpy(port_device_path, path, path_len - 1);
            port_device_path[path_len - 1] = 0;
            *out_kind = HW_OPENBCI;
            return 1;
        }
    }

    for (i = 0; i < nlim; i++) {
        char path[64];
        if (GetTickCount() - t0 > HW_DETECT_SCAN_MAX_MS) break;

        snprintf(path, sizeof(path), "\\\\.\\%s", list[i]);
        if (hw_detect_try_eeg_smt_ms(path, EEG_SMT_SCAN_PORT_MS)) {
            strncpy(port_device_path, path, path_len - 1);
            port_device_path[path_len - 1] = 0;
            *out_kind = HW_EEG_SMT;
            return 1;
        }
    }

    return 0;
}

static int stristr_contains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    size_t hl = strlen(hay);
    size_t i;
    if (nl == 0 || nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        size_t j;
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j])) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int parse_com_in_parens(const char* s, char* com, size_t com_len) {
    const char* p = s;
    while (*p) {
        if (_strnicmp(p, "(COM", 4) == 0) {
            const char* q = p + 4;
            size_t k;
            /* Friendly name contains "(COM5)" — we must copy "COM5", not only "5". */
            if (com_len < 5) return 0;
            com[0] = 'C';
            com[1] = 'O';
            com[2] = 'M';
            k = 3;
            while (isdigit((unsigned char)*q) && k + 1 < com_len) {
                com[k++] = *q++;
            }
            if (k > 3 && *q == ')') {
                com[k] = 0;
                return 1;
            }
        }
        p++;
    }
    return 0;
}

static int read_port_name_from_devreg(HDEVINFO hDevInfo, SP_DEVINFO_DATA* devInfo, char* com, size_t com_len) {
    HKEY hKey = SetupDiOpenDevRegKey(hDevInfo, devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    char pn[32];
    DWORD plen;
    DWORD typ;

    if (hKey == INVALID_HANDLE_VALUE) return 0;
    plen = (DWORD)sizeof(pn);
    typ = REG_SZ;
    if (RegQueryValueExA(hKey, "PortName", NULL, &typ, (LPBYTE)pn, &plen) != ERROR_SUCCESS || typ != REG_SZ) {
        RegCloseKey(hKey);
        return 0;
    }
    RegCloseKey(hKey);
    pn[sizeof(pn) - 1] = 0;
    strncpy(com, pn, com_len - 1);
    com[com_len - 1] = 0;
    return com[0] != 0 ? 1 : 0;
}

static int get_com_from_device(HDEVINFO hDevInfo, SP_DEVINFO_DATA* devInfo, char* com, size_t com_len) {
    DWORD dataType;
    char buf[512];
    DWORD size;
    DWORD props[2] = { SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC };
    int pi;

    for (pi = 0; pi < 2; pi++) {
        size = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, devInfo, props[pi], &dataType, (PBYTE)buf, sizeof(buf), &size))
            continue;
        buf[sizeof(buf) - 1] = 0;
        if (parse_com_in_parens(buf, com, com_len)) return 1;
    }
    return read_port_name_from_devreg(hDevInfo, devInfo, com, com_len);
}

static int dev_name_suggests_openbci(HDEVINFO hDevInfo, SP_DEVINFO_DATA* devInfo) {
    DWORD dataType;
    char buf[512];
    DWORD size;
    DWORD props[3] = { SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC, SPDRP_MFG };
    int pi;

    for (pi = 0; pi < 3; pi++) {
        size = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, devInfo, props[pi], &dataType, (PBYTE)buf, sizeof(buf), &size))
            continue;
        buf[sizeof(buf) - 1] = 0;
        if (stristr_contains(buf, "openbci")) return 1;
    }
    return 0;
}

/* Olimex EEG-SMT / OpenEEG uses the same FTDI/CP210x/CH340 USB-UART chips as many Cyton dongles. */
static int dev_name_suggests_eeg_smt(HDEVINFO hDevInfo, SP_DEVINFO_DATA* devInfo) {
    DWORD dataType;
    char buf[512];
    DWORD size;
    DWORD props[3] = { SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC, SPDRP_MFG };
    int pi;

    for (pi = 0; pi < 3; pi++) {
        size = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, devInfo, props[pi], &dataType, (PBYTE)buf, sizeof(buf), &size))
            continue;
        buf[sizeof(buf) - 1] = 0;
        if (stristr_contains(buf, "olimex")) return 1;
        if (stristr_contains(buf, "eeg-smt")) return 1;
        if (stristr_contains(buf, "eeg smt")) return 1;
        if (stristr_contains(buf, "openeeg")) return 1;
    }
    return 0;
}

/* FTDI, Silicon Labs CP210x, WinChipHead CH340 — common USB-serial on Cyton dongles / cheap adapters. */
static int instance_id_usb_uart_cyton_hint(const char* id) {
    if (!id || !id[0]) return 0;
    if (stristr_contains(id, "VID_0403") && (stristr_contains(id, "PID_6001") ||
        stristr_contains(id, "PID_6015") || stristr_contains(id, "PID_6010") || stristr_contains(id, "PID_6014")))
        return 1;
    if (stristr_contains(id, "VID_10C4") && (stristr_contains(id, "PID_EA60") ||
        stristr_contains(id, "PID_EA63") || stristr_contains(id, "PID_EA61")))
        return 1;
    if (stristr_contains(id, "VID_1A86") && stristr_contains(id, "PID_7523"))
        return 1;
    return 0;
}

int hw_detect_com_openbci_from_system(char* port_device_path, size_t path_len, int* trust_without_banner_out) {
    HDEVINFO hDevInfo;
    SP_DEVINFO_DATA devInfo;
    DWORD idx;
    char com[32];
    char instanceId[256];

    if (!port_device_path || path_len < 12) return 0;
    port_device_path[0] = 0;
    if (trust_without_banner_out) *trust_without_banner_out = 0;

    hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return 0;

    devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

    for (idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devInfo); idx++) {
        if (!dev_name_suggests_openbci(hDevInfo, &devInfo)) continue;
        if (!get_com_from_device(hDevInfo, &devInfo, com, sizeof(com))) continue;
        snprintf(port_device_path, path_len, "\\\\.\\%s", com);
        SetupDiDestroyDeviceInfoList(hDevInfo);
        if (trust_without_banner_out) *trust_without_banner_out = 1;
        return 1;
    }

    for (idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devInfo); idx++) {
        if (!SetupDiGetDeviceInstanceIdA(hDevInfo, &devInfo, instanceId, sizeof(instanceId), NULL))
            continue;
        instanceId[sizeof(instanceId) - 1] = 0;
        if (!instance_id_usb_uart_cyton_hint(instanceId)) continue;
        if (dev_name_suggests_eeg_smt(hDevInfo, &devInfo)) continue;
        if (!get_com_from_device(hDevInfo, &devInfo, com, sizeof(com))) continue;
        snprintf(port_device_path, path_len, "\\\\.\\%s", com);
        SetupDiDestroyDeviceInfoList(hDevInfo);
        /* Do not set trust_without_banner_out: generic VID/PID matches EEG-SMT adapters too.
         * Caller must still pass hw_detect_try_openbci_quick (Cyton ASCII banner). */
        return 1;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return 0;
}
