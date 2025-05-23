#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <wingdi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include "serial.h"
#include "recording.h"

#define MAX_POINTS 512
#define ID_FILE_EXIT 9001
#define ID_HELP_ABOUT 9002
#define ID_HELP_CONTENTS 9003
#define ID_FILE_START_RECORDING 9004
#define ID_FILE_STOP_RECORDING 9005
#define ID_BAND_ALL   9100
#define ID_BAND_DELTA 9101
#define ID_BAND_THETA 9102
#define ID_BAND_ALPHA 9103
#define ID_BAND_BETA  9104
#define ID_BAND_GAMMA 9105
#define SAMPLE_RATE 256

float data[MAX_POINTS][2];
float filtered_data[MAX_POINTS][2];
int data_index = 0;
int filtered_data_base_index = 0;
CRITICAL_SECTION cs_filtered_data;
HANDLE hSerial;

typedef enum {
    BAND_ALL,
    BAND_DELTA,
    BAND_THETA,
    BAND_ALPHA,
    BAND_BETA,
    BAND_GAMMA
} FrequencyBand;

FrequencyBand current_band = BAND_ALL; // Default

void get_band_limits(FrequencyBand band, float* low_hz, float* high_hz) {
    switch (band) {
        case BAND_DELTA: *low_hz = 0.5f; *high_hz = 4.0f; break;
        case BAND_THETA: *low_hz = 4.0f; *high_hz = 8.0f; break;
        case BAND_ALPHA: *low_hz = 8.0f; *high_hz = 13.0f; break;
        case BAND_BETA:  *low_hz = 13.0f; *high_hz = 30.0f; break;
        case BAND_GAMMA: *low_hz = 30.0f; *high_hz = 100.0f; break;
        default:         *low_hz = 0.0f; *high_hz = SAMPLE_RATE / 2.0f; break;
    }
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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

        push_sample(ch1, ch2);
        InvalidateRect((HWND)lpParam, NULL, FALSE);
    }
    return 0;
}

void fft_cooley_tukey(complex float* x, complex float* X, int N) {
    if (N <= 1) {
        X[0] = x[0];
        return;
    }

    int half = N / 2;
    complex float even[half], odd[half];
    for (int i = 0; i < half; i++) {
        even[i] = x[2 * i];
        odd[i] = x[2 * i + 1];
    }

    complex float even_fft[half], odd_fft[half];
    fft_cooley_tukey(even, even_fft, half);
    fft_cooley_tukey(odd, odd_fft, half);

    for (int k = 0; k < half; k++) {
        complex float t = cexpf(-2.0f * I * (float)M_PI * k / N) * odd_fft[k];
        X[k] = even_fft[k] + t;
        X[k + half] = even_fft[k] - t;
    }
}

void ifft_cooley_tukey(complex float* X, complex float* x, int N) {
    complex float X_conj[N];
    for (int i = 0; i < N; i++) X_conj[i] = conjf(X[i]);

    complex float x_temp[N];
    fft_cooley_tukey(X_conj, x_temp, N);

    for (int i = 0; i < N; i++) x[i] = conjf(x_temp[i]) / N;
}

void compute_band_filtered_from(float in[][2], float out[][2], int ch, float low_hz, float high_hz) {
    int N = MAX_POINTS;
    complex float x[MAX_POINTS];
    complex float X[MAX_POINTS] = {0};
    complex float Y[MAX_POINTS] = {0};
    complex float y[MAX_POINTS];

    for (int i = 0; i < N; ++i) {
        x[i] = in[i][ch] - 512.0f;  // remove DC bias
    }

    fft_cooley_tukey(x, X, N);

    float freq_res = (float)SAMPLE_RATE / N;
    for (int k = 0; k < N; ++k) {
        float freq = (k <= N / 2) ? k * freq_res : (k - N) * freq_res;
        if (freq >= low_hz && freq <= high_hz) {
            Y[k] = X[k];
        }
    }

    ifft_cooley_tukey(Y, y, N);

    for (int i = 0; i < N; ++i) {
        out[i][ch] = crealf(y[i]) + 512.0f;  // re-add offset
    }
}

DWORD WINAPI FilteringThread(LPVOID lpParam) {
    float local_data[MAX_POINTS][2];
    float temp_filtered[MAX_POINTS][2];
    float low_hz = 0.0f, high_hz = 128.0f;
    static int stable_base_index = 0;

    while (1) {
        if (current_band == BAND_ALL) continue;

        get_band_limits(current_band, &low_hz, &high_hz);

        EnterCriticalSection(&cs_filtered_data);
        int base_index = (data_index + MAX_POINTS - 1) % MAX_POINTS;  // lock oldest sample in view
        for (int i = 0; i < MAX_POINTS; ++i) {
            int idx = (base_index + i) % MAX_POINTS;
            local_data[i][0] = data[idx][0];
            local_data[i][1] = data[idx][1];
        }
        LeaveCriticalSection(&cs_filtered_data);

        compute_band_filtered_from(local_data, temp_filtered, 0, low_hz, high_hz);
        compute_band_filtered_from(local_data, temp_filtered, 1, low_hz, high_hz);

        EnterCriticalSection(&cs_filtered_data);
        memcpy(filtered_data, temp_filtered, sizeof(filtered_data));
        filtered_data_base_index = base_index;
        LeaveCriticalSection(&cs_filtered_data);
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

            int textY = y;
            if (i == 0) textY -= 4;
            else if (i == 4) textY += 4;

            wchar_t label[32];
            swprintf(label, L"%.0f", 1024 - value);
            SetTextColor(hdc, RGB(64, 64, 64));
            TextOutW(hdc, 5, textY - 8, label, wcslen(label));
        }

        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        HPEN pen = CreatePen(PS_SOLID, 1, colors[ch]);
        oldPen = (HPEN)SelectObject(hdc, pen);

        if (current_band == BAND_ALL) {
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
        } else {
            EnterCriticalSection(&cs_filtered_data);
            for (int i = 0; i < MAX_POINTS - 1; ++i) {
                float v1 = filtered_data[i][ch];
                float v2 = filtered_data[i + 1][ch];

                float norm1 = (v1 - roundedMin) / displayRange;
                float norm2 = (v2 - roundedMin) / displayRange;

                int x1 = leftMargin + (i * plotWidth) / MAX_POINTS;
                int x2 = leftMargin + ((i + 1) * plotWidth) / MAX_POINTS;

                int y1 = yBase + (int)(norm1 * channelHeight);
                int y2 = yBase + (int)(norm2 * channelHeight);

                MoveToEx(hdc, x1, y1, NULL);
                LineTo(hdc, x2, y2);
            }
            LeaveCriticalSection(&cs_filtered_data);
        }

        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        SetTextColor(hdc, colors[ch]);
        const char* label = (ch == 0) ? "CH1" : "CH2";
        TextOutA(hdc, width - 35, yBase + 10, label, 3);

        if (ch == 0) {
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
            oldPen = (HPEN)SelectObject(hdc, sepPen);

            int separatorY = yBase + channelHeight + (channelPadding / 2);
            MoveToEx(hdc, leftMargin, separatorY, NULL);
            LineTo(hdc, width, separatorY);

            SelectObject(hdc, oldPen);
            DeleteObject(sepPen);
        }
    }
}

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

    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenu(hFile, MF_STRING, ID_FILE_START_RECORDING, "Start Recording");
    AppendMenu(hFile, MF_STRING, ID_FILE_STOP_RECORDING, "Stop Recording");
    AppendMenu(hFile, MF_STRING, ID_FILE_EXIT, "Exit");

    HMENU hBand = CreatePopupMenu();
    AppendMenu(hBand, MF_STRING, ID_BAND_ALL,   "All");
    AppendMenu(hBand, MF_STRING, ID_BAND_DELTA, "Delta (0.5-4 Hz)");
    AppendMenu(hBand, MF_STRING, ID_BAND_THETA, "Theta (4-8 Hz)");
    AppendMenu(hBand, MF_STRING, ID_BAND_ALPHA, "Alpha (8-13 Hz)");
    AppendMenu(hBand, MF_STRING, ID_BAND_BETA,  "Beta (13-30 Hz)");
    AppendMenu(hBand, MF_STRING, ID_BAND_GAMMA, "Gamma (30-100 Hz)");

    HMENU hHelp = CreatePopupMenu();
    AppendMenu(hHelp, MF_STRING, ID_HELP_CONTENTS, "Contents");
    AppendMenu(hHelp, MF_STRING, ID_HELP_ABOUT, "About");

    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFile, "File");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hBand, "Band");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "Help");
    SetMenu(hwnd, hMenuBar);

    for (int i = ID_BAND_ALL; i <= ID_BAND_GAMMA; ++i) {
        CheckMenuItem(hMenuBar, i, MF_BYCOMMAND | ((i == (ID_BAND_ALL + current_band)) ? MF_CHECKED : MF_UNCHECKED)); // initialize the active menu item
    }

    InitializeCriticalSection(&cs_filtered_data);
    CreateThread(NULL, 0, FilteringThread, NULL, 0, NULL);
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);
        draw_plot(memDC, &rect);
        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
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
        case ID_FILE_START_RECORDING:
            if (!is_recording()) {
                init_recording();
                MessageBox(hwnd, "Recording into eeg_data.csv has started.", "Info", MB_OK);
            }
            break;
        case ID_FILE_STOP_RECORDING:
            if (is_recording()) {
                stop_recording();
                MessageBox(hwnd, "Recording has stopped. Check the eeg_data.csv file.", "Info", MB_OK);
            }
            break;
        case ID_BAND_ALL:
        case ID_BAND_DELTA:
        case ID_BAND_THETA:
        case ID_BAND_ALPHA:
        case ID_BAND_BETA:
        case ID_BAND_GAMMA:
            current_band = (FrequencyBand)(LOWORD(wParam) - ID_BAND_ALL);
            for (int i = ID_BAND_ALL; i <= ID_BAND_GAMMA; ++i) {
                CheckMenuItem(GetMenu(hwnd), i, MF_BYCOMMAND | ((i == LOWORD(wParam)) ? MF_CHECKED : MF_UNCHECKED));
            }
            break;

        case ID_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"Local Neural Monitoring v 0.2.0\n"
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
