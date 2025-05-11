#include "serial.h"

BOOL IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0,
        &adminGroup)) {

        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

void RelaunchAsAdmin() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.nShow = SW_NORMAL;

    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(NULL,
            L"Administrator privileges are required to change system settings.",
            L"Permission Denied", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    ExitProcess(0);
}

BOOL WasPromptAlreadyShown() {
    HKEY hKey;
    DWORD value = 0, size = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_CONFIRM_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_CONFIRM_VALUE_PROMPTED, NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS && value == 1) {
            RegCloseKey(hKey);
            return TRUE;
        }
        RegCloseKey(hKey);
    }
    return FALSE;
}

BOOL WasDisableRequested() {
    HKEY hKey;
    DWORD value = 0, size = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_CONFIRM_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_CONFIRM_VALUE_DISABLE, NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS && value == 1) {
            RegCloseKey(hKey);
            return TRUE;
        }
        RegCloseKey(hKey);
    }
    return FALSE;
}

void MarkPromptAsShown(BOOL disableRequested) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_CONFIRM_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD prompted = 1;
        RegSetValueExW(hKey, REG_CONFIRM_VALUE_PROMPTED, 0, REG_DWORD, (const BYTE*)&prompted, sizeof(prompted));
        DWORD disable = disableRequested ? 1 : 0;
        RegSetValueExW(hKey, REG_CONFIRM_VALUE_DISABLE, 0, REG_DWORD, (const BYTE*)&disable, sizeof(disable));
        RegCloseKey(hKey);
    }
}

void DisableSerialMouseIfRequested() {
    if (!WasDisableRequested()) return;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\sermouse", 0,
        KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {

        DWORD newValue = 4;
        if (RegSetValueExW(hKey, L"Start", 0, REG_DWORD,
            (const BYTE*)&newValue, sizeof(newValue)) == ERROR_SUCCESS) {

            MessageBoxW(NULL,
                L"Serial mouse driver has been disabled.\n\n"
                L"Please restart your computer for the change to take effect.",
                L"Change Applied", MB_OK | MB_ICONINFORMATION);
            ExitProcess(0);
        } else {
            MessageBoxW(NULL,
                L"Failed to change the serial mouse setting.",
                L"Error", MB_OK | MB_ICONERROR);
            ExitProcess(1);
        }
        RegCloseKey(hKey);
    }
}

void CheckAndDisableSerialMouse() {
    if (WasPromptAlreadyShown()) {
        if (WasDisableRequested() && IsRunningAsAdmin()) {
            DisableSerialMouseIfRequested();
        }
        return;
    }

    HKEY hKey;
    DWORD startValue = 0;
    DWORD dataSize = sizeof(DWORD);
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\sermouse", 0,
        KEY_QUERY_VALUE, &hKey);

    if (result != ERROR_SUCCESS) return;

    result = RegQueryValueExW(hKey, L"Start", NULL, NULL,
        (LPBYTE)&startValue, &dataSize);

    RegCloseKey(hKey);

    if (result == ERROR_SUCCESS && startValue != 4) {
        int response = MessageBoxW(NULL,
            L"The serial mouse driver is currently enabled.\n\n"
            L"This can interfere with the EEG-SMT device.\n\n"
            L"Are you using a serial mouse?\n\n"
            L"If you are unsure, choose 'No' (recommended).",
            L"Disable Serial Mouse?", MB_YESNO | MB_ICONQUESTION);

        BOOL disable = (response == IDNO);
        MarkPromptAsShown(disable);

        if (disable) {
            if (!IsRunningAsAdmin()) {
                RelaunchAsAdmin();
            } else {
                DisableSerialMouseIfRequested();
            }
        }
    }
}
