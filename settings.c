#define _CRT_SECURE_NO_WARNINGS
#include "settings.h"
#include "resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>

#define INI_SECTION "ports"
#define INI_KEY_SERIAL "Serial"
/* Legacy keys — read for migration; removed on save. */
#define INI_KEY_EEG "EEG_SMT"
#define INI_KEY_OBCI "OpenBCI"

static int com_name_compare(const void* a, const void* b) {
    const char* sa = (const char*)a;
    const char* sb = (const char*)b;
    int na = -1, nb = -1;
    if (_strnicmp(sa, "COM", 3) == 0) na = atoi(sa + 3);
    if (_strnicmp(sb, "COM", 3) == 0) nb = atoi(sb + 3);
    if (na < 0 && nb < 0) return _stricmp(sa, sb);
    if (na < 0) return -1;
    if (nb < 0) return 1;
    if (na != nb) return (na > nb) - (na < nb);
    return _stricmp(sa, sb);
}

void settings_ini_path(char* buf, size_t buf_len) {
    char base[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, base) != S_OK) {
        GetModuleFileNameA(NULL, base, MAX_PATH);
        char* p = strrchr(base, '\\');
        if (p) *p = 0;
    }
    snprintf(buf, buf_len, "%s\\LocalNeuralMonitoring", base);
    CreateDirectoryA(buf, NULL);
    snprintf(buf, buf_len, "%s\\LocalNeuralMonitoring\\settings.ini", base);
}

void settings_load(char* port_out, size_t name_len) {
    char path[MAX_PATH];
    char legacy[SETTINGS_COM_NAME_LEN];

    settings_ini_path(path, sizeof(path));
    GetPrivateProfileStringA(INI_SECTION, INI_KEY_SERIAL, "", port_out, (DWORD)name_len, path);
    if (port_out[0])
        return;

    GetPrivateProfileStringA(INI_SECTION, INI_KEY_EEG, "", legacy, sizeof(legacy), path);
    if (legacy[0]) {
        strncpy(port_out, legacy, name_len - 1);
        port_out[name_len - 1] = 0;
        return;
    }
    GetPrivateProfileStringA(INI_SECTION, INI_KEY_OBCI, "", port_out, (DWORD)name_len, path);
}

void settings_save(const char* port) {
    char path[MAX_PATH];
    settings_ini_path(path, sizeof(path));
    WritePrivateProfileStringA(INI_SECTION, INI_KEY_SERIAL, port, path);
    WritePrivateProfileStringA(INI_SECTION, INI_KEY_EEG, NULL, path);
    WritePrivateProfileStringA(INI_SECTION, INI_KEY_OBCI, NULL, path);
}

int settings_enum_ports(char (*names)[SETTINGS_COM_NAME_LEN], int max_names) {
    HKEY hk;
    int count = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;

    for (DWORD idx = 0; count < max_names; idx++) {
        char valueName[256];
        DWORD vnlen = sizeof(valueName);
        BYTE data[SETTINGS_COM_NAME_LEN];
        DWORD dlen = sizeof(data) - 1;
        DWORD typ = 0;
        LONG r = RegEnumValueA(hk, idx, valueName, &vnlen, NULL, &typ, data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (typ != REG_SZ && typ != REG_EXPAND_SZ) continue;
        data[sizeof(data) - 1] = 0;
        if (dlen == 0) continue;
        strncpy(names[count], (char*)data, SETTINGS_COM_NAME_LEN - 1);
        names[count][SETTINGS_COM_NAME_LEN - 1] = 0;
        count++;
    }
    RegCloseKey(hk);

    if (count > 1)
        qsort(names, (size_t)count, SETTINGS_COM_NAME_LEN, com_name_compare);
    return count;
}

int settings_port_in_enum(const char* com_name) {
    char list[64][SETTINGS_COM_NAME_LEN];
    int n = settings_enum_ports(list, 64);
    int i;
    for (i = 0; i < n; i++) {
        if (_stricmp(com_name, list[i]) == 0) return 1;
    }
    return 0;
}

void settings_auto_pick(char* out, size_t out_len) {
    char list[64][SETTINGS_COM_NAME_LEN];
    int n = settings_enum_ports(list, 64);
    out[0] = 0;
    if (n <= 0) return;
    if (n == 1) {
        strncpy(out, list[0], out_len - 1);
        out[out_len - 1] = 0;
        return;
    }
    strncpy(out, list[n - 1], out_len - 1);
    out[out_len - 1] = 0;
}

void com_name_to_device_path(const char* com_name, char* path_out, size_t path_len) {
    if (!com_name || !com_name[0]) {
        path_out[0] = 0;
        return;
    }
    if (_strnicmp(com_name, "\\\\.\\", 4) == 0)
        snprintf(path_out, path_len, "%s", com_name);
    else
        snprintf(path_out, path_len, "\\\\.\\%s", com_name);
}

void settings_resolve_port(char* port_out, size_t port_len, int cmdline_overrode_port) {
    char work[SETTINGS_COM_NAME_LEN];

    if (cmdline_overrode_port) {
        strncpy(work, port_out, sizeof(work) - 1);
        work[sizeof(work) - 1] = 0;
    } else {
        settings_load(work, sizeof(work));
    }

    if (!cmdline_overrode_port && (!work[0] || !settings_port_in_enum(work)))
        settings_auto_pick(work, sizeof(work));

    if (!work[0])
        strncpy(work, "COM1", sizeof(work) - 1);

    strncpy(port_out, work, port_len - 1);
    port_out[port_len - 1] = 0;
}

/* ---- modal settings dialog (template in resources.rc) ---- */

#define WM_SETTINGS_DETECT_DONE (WM_APP + 52)
#define IDT_SETTINGS_ANIM       2

static HWND g_cb_port;
static HWND g_hwnd_status;
static HCURSOR g_prev_class_cursor;
static int g_detect_busy;
static unsigned g_detect_anim_phase;

static void settings_dialog_fill_combo(HWND cb, const char* select) {
    char list[64][SETTINGS_COM_NAME_LEN];
    int n = settings_enum_ports(list, 64);
    int i;
    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++)
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)list[i]);
    if (n == 0) {
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"(no COM ports)");
        SendMessageA(cb, CB_SETCURSEL, 0, 0);
        return;
    }
    {
        int idx = (int)SendMessageA(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)select);
        if (idx >= 0)
            SendMessageA(cb, CB_SETCURSEL, (WPARAM)idx, 0);
        else
            SendMessageA(cb, CB_SETCURSEL, (WPARAM)(n - 1), 0);
    }
}

static void settings_dialog_autodetect_fallback(void) {
    char port[SETTINGS_COM_NAME_LEN];
    settings_load(port, sizeof(port));
    if (!port[0] || !settings_port_in_enum(port)) settings_auto_pick(port, sizeof(port));
    settings_dialog_fill_combo(g_cb_port, port);
}

static const char* settings_device_display_name(HardwareKind hk) {
    if (hk == HW_OPENBCI)
        return "OpenBCI Cyton Board";
    return "Olimex EEG-SMT";
}

static DWORD WINAPI settings_detect_thread(LPVOID p) {
    HWND hwnd = (HWND)p;
    HWND owner;
    SettingsDetectResult* r;

    r = (SettingsDetectResult*)malloc(sizeof(SettingsDetectResult));
    if (!r) {
        if (IsWindow(hwnd))
            PostMessageA(hwnd, WM_SETTINGS_DETECT_DONE, 0, 0);
        return 0;
    }
    memset(r, 0, sizeof(*r));

    owner = GetParent(hwnd);
    if (!owner)
        owner = GetWindow(hwnd, GW_OWNER);
    if (owner)
        SendMessageA(owner, WM_APP_SETTINGS_PAUSE_SERIAL, 0, 0);

    /* Same strategy as startup HardwareDetectThread: SetupAPI hint + trust, then full scan.
     * COM port must be closed first — Windows exclusive open blocks probe while app holds it. */
    {
        int trust_skip = 0;
        if (hw_detect_com_openbci_from_system(r->path, sizeof(r->path), &trust_skip) &&
            (trust_skip || hw_detect_try_openbci_quick(r->path))) {
            r->found = 1;
            r->hk = HW_OPENBCI;
            if (IsWindow(hwnd))
                PostMessageA(hwnd, WM_SETTINGS_DETECT_DONE, 0, (LPARAM)r);
            else
                free(r);
            return 0;
        }
    }
    r->found = hw_detect_scan(r->path, sizeof(r->path), &r->hk);
    if (IsWindow(hwnd))
        PostMessageA(hwnd, WM_SETTINGS_DETECT_DONE, 0, (LPARAM)r);
    else
        free(r);
    return 0;
}

static void settings_dialog_detect_enable(HWND hwnd, int on) {
    EnableWindow(GetDlgItem(hwnd, IDOK), on);
    EnableWindow(GetDlgItem(hwnd, IDCANCEL), on);
    EnableWindow(GetDlgItem(hwnd, ID_BTN_AUTO), on);
    EnableWindow(g_cb_port, on);
}

static void settings_dialog_begin_detect(HWND hwnd) {
    HANDLE th;

    if (g_detect_busy)
        return;

    g_detect_busy = 1;
    g_detect_anim_phase = 0;
    SetWindowTextA(g_hwnd_status, "Detecting");
    settings_dialog_detect_enable(hwnd, FALSE);
    g_prev_class_cursor = (HCURSOR)GetClassLongPtr(hwnd, GCLP_HCURSOR);
    SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)LoadCursor(NULL, IDC_WAIT));
    SetTimer(hwnd, IDT_SETTINGS_ANIM, 400, NULL);

    th = CreateThread(NULL, 0, settings_detect_thread, hwnd, 0, NULL);
    if (th)
        CloseHandle(th);
    else {
        KillTimer(hwnd, IDT_SETTINGS_ANIM);
        SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)g_prev_class_cursor);
        g_detect_busy = 0;
        settings_dialog_detect_enable(hwnd, TRUE);
        SetWindowTextA(g_hwnd_status, "");
        MessageBoxA(hwnd, "Could not start detection thread.", "Settings", MB_OK | MB_ICONERROR);
    }
}

static void settings_dialog_end_detect(HWND hwnd) {
    KillTimer(hwnd, IDT_SETTINGS_ANIM);
    SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)g_prev_class_cursor);
    g_detect_busy = 0;
    settings_dialog_detect_enable(hwnd, TRUE);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETTINGS_DETECT_DONE) {
        SettingsDetectResult* r = (SettingsDetectResult*)lParam;
        char status[256];
        HWND owner = GetParent(hwnd);
        if (!owner)
            owner = GetWindow(hwnd, GW_OWNER);

        settings_dialog_end_detect(hwnd);

        if (r && r->found) {
            const char* dev = settings_device_display_name(r->hk);
            const char* port_disp = hw_detect_port_display_name(r->path);
            settings_dialog_fill_combo(g_cb_port, port_disp);
            snprintf(status, sizeof(status), "Detected: %s on %s", dev, port_disp);
            SetWindowTextA(g_hwnd_status, status);
        } else {
            SetWindowTextA(g_hwnd_status, "Not found.");
            settings_dialog_autodetect_fallback();
        }

        if (owner)
            SendMessageA(owner, WM_APP_SETTINGS_RESUME_SERIAL, 0, (LPARAM)r);
        else if (r)
            free(r);
        return TRUE;
    }

    switch (msg) {
    case WM_INITDIALOG:
        g_cb_port = GetDlgItem(hwnd, ID_COMBO_PORT);
        g_hwnd_status = GetDlgItem(hwnd, ID_STATIC_STATUS);
        g_detect_busy = 0;
        {
            char port[SETTINGS_COM_NAME_LEN];
            const char* prefer = (const char*)lParam;
            settings_load(port, sizeof(port));
            if (prefer && prefer[0] && settings_port_in_enum(prefer)) {
                strncpy(port, prefer, sizeof(port) - 1);
                port[sizeof(port) - 1] = 0;
            } else if (!port[0] || !settings_port_in_enum(port))
                settings_auto_pick(port, sizeof(port));
            settings_dialog_fill_combo(g_cb_port, port);
            if (prefer && prefer[0] && settings_port_in_enum(prefer))
                SetWindowTextA(g_hwnd_status, "Current connection (change port or use Auto-detect).");
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == IDT_SETTINGS_ANIM && g_detect_busy) {
            static const char* const anim[] = {
                "Detecting", "Detecting.", "Detecting..", "Detecting..."
            };
            g_detect_anim_phase = (g_detect_anim_phase + 1u) % 4u;
            SetWindowTextA(g_hwnd_status, anim[g_detect_anim_phase]);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_AUTO:
            settings_dialog_begin_detect(hwnd);
            return TRUE;

        case IDOK: {
            char port[SETTINGS_COM_NAME_LEN];
            int ip = (int)SendMessageA(g_cb_port, CB_GETCURSEL, 0, 0);
            if (ip >= 0)
                SendMessageA(g_cb_port, CB_GETLBTEXT, (WPARAM)ip, (LPARAM)port);
            else
                port[0] = 0;
            if (port[0] && _stricmp(port, "(no COM ports)") != 0)
                settings_save(port);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR settings_show_dialog(HWND owner, HINSTANCE inst, const char* prefer_com_name) {
    return DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_SETTINGS), owner, SettingsDlgProc,
                           (LPARAM)(prefer_com_name ? prefer_com_name : NULL));
}
