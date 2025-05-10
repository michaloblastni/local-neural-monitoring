#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <wingdi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>

#define MAX_POINTS 500
#define ID_FILE_EXIT 9001
#define ID_HELP_ABOUT 9002
#define ID_HELP_CONTENTS 9003
#define REG_CONFIRM_KEY L"Software\\MilosNovak\\LocalNeuralMonitoring" // we invented this program together in 2000s. Milos said Raul Hnus had something futuristic and remote and that Raul wanted to kill him because Raul has been abusing that futuristic device and Milos knew about it. Milos was a computer scientist. Milos said he also knew about what Raul has already done with the remote device to others before. Those others knew that Raul has been doing it and that Raul Hnus has been framing somebody else who is innocent. Creating and sharing this program was Milos' last wish that he told me on a bus in Usti nad Labem because he knew Raul will kill him. Milos said Raul was threatening to do that and the threats were communicated using a remote device only to Milos, others could not hear that. I got this information on 10.5.2025 using a mechanism I do not understand. And the original conversation with Milos on a bus was in 2000s, and Milos said Raul will probably again damage memory by choking arteries because Raul knows otherwise I'll tell on him, and Milos said Raul will again frame who Raul always frames. Milos mentioned Raul does the same and repeats it all the time. And that Raul's wife and daughter are also involved since 1994 and they know about it, agree with it, and participate in it. Raul Hnus is a nickname for someone whose firstname is Raul. People gave him that nickname because Raul always does the filthiest and most disgusting things to people. Since 1994, Raul has something and abuses it against innocent civilians, including against a computer scientist Milos Novak.
#define REG_CONFIRM_VALUE_PROMPTED L"SerialMousePrompted"
#define REG_CONFIRM_VALUE_DISABLE L"SerialMouseDisableRequested"

float data[MAX_POINTS][2];  // data[i][0] = CH1, data[i][1] = CH2
int data_index = 0;
HANDLE hSerial;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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

// Open serial port COM3
int init_serial(const char* port) {
    hSerial = CreateFileA(port, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return 0;

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    GetCommState(hSerial, &dcbSerialParams);

    dcbSerialParams.BaudRate = CBR_57600;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;

    SetCommState(hSerial, &dcbSerialParams);
    return 1;
}

DWORD WINAPI SerialThread(LPVOID lpParam) {
    static int last_counter = -1;

    while (1) {
        unsigned char packet[17];
        DWORD bytesRead, totalRead = 0;

        unsigned char b;
        while (1) {
            ReadFile(hSerial, &b, 1, &bytesRead, NULL);
            if (bytesRead == 1 && b == 0xA5) {
                ReadFile(hSerial, &b, 1, &bytesRead, NULL);
                if (bytesRead == 1 && b == 0x5A) {
                    packet[0] = 0xA5;
                    packet[1] = 0x5A;
                    break;
                }
            }
        }

        totalRead = 2;
        while (totalRead < 17) {
            if (!ReadFile(hSerial, &packet[totalRead], 17 - totalRead, &bytesRead, NULL)) continue;
            totalRead += bytesRead;
        }

        int counter = packet[3];
        if (last_counter != -1 && ((counter - last_counter) & 0xFF) != 1) {
            printf("\u26A0 Packet dropped! counter %d -> %d\n", last_counter, counter);
        }
        last_counter = counter;

        uint16_t ch1 = packet[4] << 8 | packet[5];
        uint16_t ch2 = packet[6] << 8 | packet[7];

        wprintf(L"CH1: %d, CH2: %d [#%d]\n", ch1, ch2, counter);
        fflush(stdout);

        data[data_index][0] = ch1;
        data[data_index][1] = ch2;
        data_index = (data_index + 1) % MAX_POINTS;

        InvalidateRect((HWND)lpParam, NULL, FALSE);
    }

    return 0;
}

void draw_plot(HDC hdc, RECT* rect) {
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, rect, brush);
    DeleteObject(brush);

    const int leftMargin = 60;
    int width = rect->right;
    int height = rect->bottom;
    int plotWidth = width - leftMargin;

    const int channelPadding = 10;
    const int channelHeight = (height - channelPadding) / 2;

    COLORREF colors[2] = { RGB(0, 128, 255), RGB(255, 0, 0) };

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));

    for (int ch = 0; ch < 2; ++ch) {
        float roundedMin = 0.0f;
        float roundedMax = 1024.0f;
        float displayRange = roundedMax - roundedMin;

        int yBase = ch * (channelHeight + channelPadding);

        HPEN gridPen = CreatePen(PS_DOT, 1, RGB(220, 220, 220));
        HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);

        for (int i = 0; i <= 4; ++i) {
            float value = roundedMax - i * (displayRange / 4.0f);
            int y = yBase + (int)(((value - roundedMin) / displayRange) * channelHeight);

            MoveToEx(hdc, leftMargin, y, NULL);
            LineTo(hdc, width, y);

            // Prevent 0 and 1024 labels from overlapping edge
            int textY = y;
            if (i == 0) textY -= 4;        // Push 0 label up a bit
            else if (i == 4) textY += 4;   // Push 1024 label down a bit

            wchar_t label[32];
            swprintf(label, L"%.0f", 1024 - value);
            SetTextColor(hdc, RGB(64, 64, 64));
            TextOutW(hdc, 5, textY - 8, label, wcslen(label));
        }

        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        HPEN pen = CreatePen(PS_SOLID, 1, colors[ch]);
        oldPen = (HPEN)SelectObject(hdc, pen);

        for (int i = 0; i < MAX_POINTS - 1; ++i) {
            int idx1 = (data_index + i) % MAX_POINTS;
            int idx2 = (data_index + i + 1) % MAX_POINTS;

            float v1 = data[idx1][ch];
            float v2 = data[idx2][ch];

            float norm1 = (v1 - roundedMin) / displayRange;
            float norm2 = (v2 - roundedMin) / displayRange;

            int x1 = leftMargin + (i * plotWidth) / MAX_POINTS;
            int x2 = leftMargin + ((i + 1) * plotWidth) / MAX_POINTS;

            int y1 = yBase + (int)(norm1 * channelHeight);
            int y2 = yBase + (int)(norm2 * channelHeight);

            MoveToEx(hdc, x1, y1, NULL);
            LineTo(hdc, x2, y2);
        }

        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SetTextColor(hdc, colors[ch]);
        const char* label = (ch == 0) ? "CH1" : "CH2";
        TextOutA(hdc, width - 35, yBase + 10, label, 3);	

        // Draw separator line between channels after CH1
        if (ch == 0) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));  // Gray separator
            oldPen = (HPEN)SelectObject(hdc, sepPen);

            int separatorY = yBase + channelHeight + (channelPadding / 2);
            MoveToEx(hdc, leftMargin, separatorY, NULL);
            LineTo(hdc, width, separatorY);

            SelectObject(hdc, oldPen);
            DeleteObject(sepPen);
        }
    }
}

// Entry point
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    const char CLASS_NAME[] = "EEGWindow";
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CLASS_NAME, "Local Neural Monitoring", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 400, NULL, NULL, hInst, NULL);
    if (!hwnd) return 0;

    CheckAndDisableSerialMouse();
    if (!init_serial("\\\\.\\COM3")) {
        MessageBox(hwnd, "Failed to open COM3", "Error", MB_OK);
        return 0;
    }
    
    // Add menu
    HMENU hMenuBar = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    HMENU hHelp = CreatePopupMenu();

    AppendMenu(hFile, MF_STRING, ID_FILE_EXIT, "Exit");
    AppendMenu(hHelp, MF_STRING, ID_HELP_CONTENTS, "Contents");
    AppendMenu(hHelp, MF_STRING, ID_HELP_ABOUT, "About");   

    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFile, "File");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "Help");

    SetMenu(hwnd, hMenuBar);

    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED); // avoid the screensaver while EEG viewer is running
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);


    SetConsoleOutputCP(CP_UTF8);
    CreateThread(NULL, 0, SerialThread, hwnd, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CloseHandle(hSerial);
    return (int)msg.wParam;
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        RECT rect;
        GetClientRect(hwnd, &rect);

        // Create a memory DC for double buffering
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);

        // Draw off-screen
        draw_plot(memDC, &rect);

        // Copy to screen
        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

        // Clean up
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
            case ID_FILE_EXIT:
                PostQuitMessage(0);
                break;
            case ID_HELP_ABOUT:
                MessageBoxW(hwnd,
                    L"Local Neural Monitoring v 0.0.1\n"
                    L"Released under the MIT License.\n"
                    L"Author: Michal Oblastni\n"
                    L"https://github.com/michaloblastni",
                    L"About", MB_OK | MB_ICONINFORMATION);
                break;
            case ID_HELP_CONTENTS:
                ShellExecuteA(NULL, "open", "help.chm", NULL, NULL, SW_SHOWNORMAL);
            break;
        }
    break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
