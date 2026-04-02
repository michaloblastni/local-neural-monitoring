#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <wingdi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>
#include "serial.h"
#include "recording.h"
#include "settings.h"
#include "hw_detect.h"
#include "resource.h"

typedef enum {
    DEVICE_EEG_SMT,
    DEVICE_OPENBCI
} DeviceKind;

static DeviceKind g_device = DEVICE_EEG_SMT;
static char g_com_port[64] = "\\\\.\\COM3";
static int g_cmdline_overrode_port = 0;
static int g_cmdline_empty = 1;
static int g_cmdline_explicit_openbci = 0;
static char g_connection_status[256];

#define MAX_POINTS 512
/* Menu command IDs are in resource.h (shared with resources.rc). */
#define EEG_SAMPLE_RATE 256

#define IDT_OPENBCI_TIMER 1

/* Bottom strip reserved for connection / status text (plot area ends above this). */
#define STATUS_BAR_HEIGHT 28

/* ---- OpenBCI (Cyton 8ch, 250 Hz, 5 s window) ---- */
#define OB_PACKET_SIZE    33
#define OB_MAX_CHANNELS   8
#define OB_SAMPLE_RATE    250
#define OB_WINDOW_SEC     5.0
/* Integer constant (250*5) — required for static array dims with MinGW GCC. */
#define OB_HIST_SAMPLES   1250

#ifndef MAXI
#define MAXI(a,b) (( (a) > (b) ) ? (a) : (b))
#endif

float data[MAX_POINTS][2];
float filtered_data[MAX_POINTS][2];
int data_index = 0;
int filtered_data_base_index = 0;
CRITICAL_SECTION cs_filtered_data;
HANDLE hSerial = INVALID_HANDLE_VALUE;

#define WM_APP_DETECT_DONE   (WM_APP + 1)
#define WM_APP_SCAN_PROGRESS (WM_APP + 53)
#define WM_APP_EEG_SAMPLE    (WM_APP + 56)

typedef struct {
    int found;
    HardwareKind hk;
    char port[64];
} DetectResult;

static int g_io_started = 0;
static int g_eeg_filter_ready = 0;
static char g_saved_com_port[64];
static DeviceKind g_saved_device_kind;
static int g_settings_detect_had_serial;
static volatile LONG g_serial_reader_stop = 0;
static HANDLE g_serial_thread_handle = NULL;
/* Serial thread coalesces redraw requests: only one WM_APP_EEG_SAMPLE queued until UI clears it. */
static LONG g_eeg_redraw_posted = 0;

/* EEG console output (non-blocking): SerialThread enqueues, writer thread prints. */
#define EEG_CONSOLE_QUEUE 4096
typedef struct {
    uint16_t ch1;
    uint16_t ch2;
    DWORD    tick_ms; /* GetTickCount() at the moment the packet was parsed */
} EegConsoleSample;
static EegConsoleSample g_console_queue[EEG_CONSOLE_QUEUE];
static int g_console_write = 0;
static int g_console_read = 0;
static HANDLE g_console_event = NULL;
static HANDLE g_console_thread = NULL;
static CRITICAL_SECTION g_console_cs;
static volatile LONG g_console_shutdown = 0;
static int g_console_ready = 0;

typedef enum {
    BAND_ALL,
    BAND_DELTA,
    BAND_THETA,
    BAND_ALPHA,
    BAND_BETA,
    BAND_GAMMA
} FrequencyBand;

FrequencyBand current_band = BAND_ALL;

static uint8_t ob_serialBuffer[4096];
static int ob_serialBufferLength = 0;
static int32_t ob_lastSample[OB_MAX_CHANNELS] = {0};
static int32_t ob_hist[OB_MAX_CHANNELS][OB_HIST_SAMPLES];
static int32_t ob_band_line[OB_MAX_CHANNELS][MAX_POINTS];
static int ob_band_cache_ok;
static int ob_writePos = 0;
static int ob_histFilled = 0;

static void ob_update_band_filter_cache(void);

void get_band_limits(FrequencyBand band, float* low_hz, float* high_hz) {
    switch (band) {
        case BAND_DELTA: *low_hz = 0.5f; *high_hz = 4.0f; break;
        case BAND_THETA: *low_hz = 4.0f; *high_hz = 8.0f; break;
        case BAND_ALPHA: *low_hz = 8.0f; *high_hz = 13.0f; break;
        case BAND_BETA:  *low_hz = 13.0f; *high_hz = 30.0f; break;
        case BAND_GAMMA: *low_hz = 30.0f; *high_hz = 100.0f; break;
        default:         *low_hz = 0.0f; *high_hz = EEG_SAMPLE_RATE / 2.0f; break;
    }
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

static void parse_cmdline(LPSTR lpCmdLine) {
    char buf[512];
    char* tok;

    g_device = DEVICE_EEG_SMT;
    g_cmdline_overrode_port = 0;
    g_cmdline_explicit_openbci = 0;
    strcpy(g_com_port, "\\\\.\\COM3");

    g_cmdline_empty = (!lpCmdLine || !lpCmdLine[0]);
    if (g_cmdline_empty) return;

    strncpy(buf, lpCmdLine, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    tok = strtok(buf, " \t");
    if (!tok) return;

    if (_stricmp(tok, "openbci") == 0) {
        g_cmdline_explicit_openbci = 1;
        g_device = DEVICE_OPENBCI;
        strcpy(g_com_port, "\\\\.\\COM5");
        tok = strtok(NULL, " \t");
        if (tok) {
            g_cmdline_overrode_port = 1;
            if (_strnicmp(tok, "\\\\.\\", 4) == 0)
                strncpy(g_com_port, tok, sizeof(g_com_port) - 1);
            else
                snprintf(g_com_port, sizeof(g_com_port), "\\\\.\\%s", tok);
            g_com_port[sizeof(g_com_port) - 1] = 0;
        }
        return;
    }

    if ((tok[0] == 'C' || tok[0] == 'c') && (tok[1] == 'O' || tok[1] == 'o')) {
        g_cmdline_overrode_port = 1;
        snprintf(g_com_port, sizeof(g_com_port), "\\\\.\\%s", tok);
    }
}

/* Try to set the FTDI VCP latency timer to 1 ms for the given COM port.
 *
 * The latency timer controls how often the FT232R/FT2232/etc. USB chip
 * flushes its internal receive FIFO to the Windows driver buffer.
 * Default is 16 ms; some systems have it at 255 ms or higher, which causes
 * the "1-second update" symptom seen in EEG applications.
 *
 * Returns: current latency-timer value (ms) found in registry, or -1 if
 * the port is not an FTDI device / key not found.
 * Side-effect: attempts to write 1 ms; prints the result to stdout. */
static int try_set_ftdi_latency_1ms(const char *port) {
    const char *name = port;
    char sub[256], path[512], pn[32];
    DWORD idx, len, type, pnlen;
    HKEY hRoot, hP;
    int found_lat = -1;

    if (_strnicmp(name, "\\\\.\\", 4) == 0) name += 4;

    /* FTDIBUS is where the FTDI VCP driver stores per-port parameters. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS",
                      0, KEY_READ, &hRoot) != ERROR_SUCCESS)
        return -1; /* not FTDI or key absent */

    for (idx = 0; ; ++idx) {
        len = sizeof(sub);
        if (RegEnumKeyExA(hRoot, idx, sub, &len,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        snprintf(path, sizeof(path),
            "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS\\%s\\0000\\Device Parameters",
            sub);

        /* First, try read-only to get the current value. */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hP) != ERROR_SUCCESS)
            continue;
        pnlen = sizeof(pn); type = 0;
        if (RegQueryValueExA(hP, "PortName", NULL, &type,
                             (LPBYTE)pn, &pnlen) != ERROR_SUCCESS
            || _stricmp(pn, name) != 0) {
            RegCloseKey(hP);
            continue;
        }
        /* Matched port — read current LatencyTimer. */
        {
            DWORD cur = 16, csz = sizeof(cur); type = 0;
            if (RegQueryValueExA(hP, "LatencyTimer", NULL, &type,
                                 (LPBYTE)&cur, &csz) == ERROR_SUCCESS)
                found_lat = (int)cur;
            else
                found_lat = 16; /* FTDI default when key absent */
        }
        RegCloseKey(hP);

        /* Already optimal — nothing to do, skip write and elevation entirely. */
        if (found_lat == 1) {
            break;
        }

        /* Value is wrong — try to correct it with write access (requires admin). */
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0,
                          KEY_READ | KEY_WRITE, &hP) == ERROR_SUCCESS) {
            DWORD lat = 1;
            LONG r = RegSetValueExA(hP, "LatencyTimer", 0, REG_DWORD,
                                    (const BYTE*)&lat, sizeof(lat));
            RegCloseKey(hP);
            if (r == ERROR_SUCCESS) {
                printf("[FTDI] LatencyTimer was %d ms, now set to 1 ms.\n", found_lat);
                found_lat = 1;
            } else {
                printf("[FTDI] LatencyTimer is %d ms; write failed (err %ld). "
                       "Will request elevation.\n", found_lat, r);
                found_lat = -2; /* signal: needs elevation */
            }
        } else {
            printf("[FTDI] LatencyTimer is %d ms; no write access. "
                   "Will request elevation.\n", found_lat);
            found_lat = -2; /* signal: needs elevation */
        }
        break;
    }
    RegCloseKey(hRoot);
    return found_lat;
}

/* Write LatencyTimer=1 directly (called when already running elevated). */
static void do_set_ftdi_latency_elevated(const char *com_name) {
    char sub[256], path[512], pn[32];
    DWORD idx, len, type, pnlen;
    HKEY hRoot, hP;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS",
                      0, KEY_READ, &hRoot) != ERROR_SUCCESS)
        return;
    for (idx = 0; ; ++idx) {
        len = sizeof(sub);
        if (RegEnumKeyExA(hRoot, idx, sub, &len,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        snprintf(path, sizeof(path),
            "SYSTEM\\CurrentControlSet\\Enum\\FTDIBUS\\%s\\0000\\Device Parameters", sub);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0,
                          KEY_READ | KEY_WRITE, &hP) != ERROR_SUCCESS)
            continue;
        pnlen = sizeof(pn); type = 0;
        if (RegQueryValueExA(hP, "PortName", NULL, &type,
                             (LPBYTE)pn, &pnlen) == ERROR_SUCCESS
            && _stricmp(pn, com_name) == 0) {
            DWORD lat = 1;
            RegSetValueExA(hP, "LatencyTimer", 0, REG_DWORD,
                           (const BYTE*)&lat, sizeof(lat));
            RegCloseKey(hP);
            break;
        }
        RegCloseKey(hP);
    }
    RegCloseKey(hRoot);
}

/* Spawn an elevated copy of ourselves with --ftdi-lat <com_name> so it can
 * write LatencyTimer=1 to HKLM.  Blocks until the child exits (UAC prompt
 * is the only interaction required). */
static void elevate_and_set_ftdi_latency(const char *com_name) {
    char exe[MAX_PATH], params[128];
    SHELLEXECUTEINFOA sei;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return;

    snprintf(params, sizeof(params), "--ftdi-lat %s", com_name);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize      = sizeof(sei);
    sei.fMask       = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb      = "runas";   /* request elevation */
    sei.lpFile      = exe;
    sei.lpParameters = params;
    sei.nShow       = SW_HIDE;

    printf("[FTDI] Requesting one-time administrator access to set LatencyTimer=1 ms...\n");
    fflush(stdout);

    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            printf("[FTDI] UAC cancelled. Set Latency Timer manually:\n"
                   "       Device Manager -> %s -> Properties -> "
                   "Port Settings -> Advanced -> Latency Timer = 1\n", com_name);
        else
            printf("[FTDI] ShellExecuteEx failed (err %lu).\n", err);
        fflush(stdout);
        return;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
        printf("[FTDI] LatencyTimer=1 ms applied. "
               "Data should now stream in real-time.\n");
        fflush(stdout);
    }
}

static int init_serial_eeg_smt(const char* port) {
    COMMTIMEOUTS to = { 0 };
    DCB dcbSerialParams = { 0 };

    /* Lower FTDI hardware latency timer before opening.
     * The new registry value is picked up by the FTDI VCP driver on the
     * CreateFileA call below (no re-plug needed on most driver versions). */
    {
        const char *name = port;
        char com_name[32] = {0};
        if (_strnicmp(name, "\\\\.\\", 4) == 0) name += 4;
        strncpy(com_name, name, sizeof(com_name) - 1);

        int lat = try_set_ftdi_latency_1ms(port);
        if (lat == -1)
            printf("[Serial] %s: not an FTDI device (FTDIBUS key not found).\n",
                   com_name);
        else if (lat == -2)
            elevate_and_set_ftdi_latency(com_name); /* UAC prompt → sets registry */
    }

    /* Open with FILE_FLAG_OVERLAPPED so that ReadFile issues an asynchronous
     * kernel-level read request.  The OS completes the request and signals the
     * OVERLAPPED event the instant ANY byte enters the driver receive buffer,
     * regardless of the chip's hardware latency timer.  This is the only
     * reliable way to achieve sub-millisecond read latency on USB-serial. */
    hSerial = CreateFileA(port, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return 0;

    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams)) {
        CloseHandle(hSerial); hSerial = INVALID_HANDLE_VALUE; return 0;
    }

    dcbSerialParams.BaudRate          = CBR_57600;
    dcbSerialParams.ByteSize          = 8;
    dcbSerialParams.StopBits          = ONESTOPBIT;
    dcbSerialParams.Parity            = NOPARITY;
    dcbSerialParams.fBinary           = TRUE;
    dcbSerialParams.fParity           = FALSE;
    dcbSerialParams.fOutxCtsFlow      = FALSE;
    dcbSerialParams.fOutxDsrFlow      = FALSE;
    dcbSerialParams.fDtrControl       = DTR_CONTROL_ENABLE;
    dcbSerialParams.fDsrSensitivity   = FALSE;
    dcbSerialParams.fTXContinueOnXoff = TRUE;
    dcbSerialParams.fOutX             = FALSE;
    dcbSerialParams.fInX              = FALSE;
    dcbSerialParams.fNull             = FALSE;
    dcbSerialParams.fRtsControl       = RTS_CONTROL_ENABLE;
    dcbSerialParams.fAbortOnError     = FALSE;

    if (!SetCommState(hSerial, &dcbSerialParams)) {
        CloseHandle(hSerial); hSerial = INVALID_HANDLE_VALUE; return 0;
    }

    /* ReadIntervalTimeout = 1 ms: the overlapped ReadFile completes after
     * 1 ms of inter-character silence.  At 57600 baud the inter-byte gap
     * within a 17-byte EEG-SMT packet is ~173 µs (well inside 1 ms), so
     * whole packets arrive in one overlapped completion.  The inter-packet
     * gap at 256 Hz is ~3.9 ms, which exceeds 1 ms and triggers the timeout.
     * ReadTotalTimeoutConstant = 100 ms: if the device sends nothing at all
     * for 100 ms the read returns empty so the thread can check the stop flag. */
    to.ReadIntervalTimeout        = 1;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant   = 100;
    SetCommTimeouts(hSerial, &to);

    SetupComm(hSerial, 1 << 14, 1 << 14);
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 1;
}

static int init_serial_openbci(const char* portName) {
    DCB dcb;
    COMMTIMEOUTS to;

    hSerial = hw_detect_open_com_app(portName, 1);
    if (hSerial == INVALID_HANDLE_VALUE) return 0;

    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hSerial, &dcb)) return 0;
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    if (!SetCommState(hSerial, &dcb)) return 0;

    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    SetCommTimeouts(hSerial, &to);

    SetupComm(hSerial, 1<<15, 1<<15);
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 1;
}

static DWORD WINAPI EegConsoleThread(LPVOID lpParam) {
    DWORD base = 0;
    (void)lpParam;
    for (;;) {
        WaitForSingleObject(g_console_event, INFINITE);
        if (InterlockedCompareExchange(&g_console_shutdown, 0, 0))
            return 0;
        for (;;) {
            EegConsoleSample s;
            EnterCriticalSection(&g_console_cs);
            if (g_console_read == g_console_write) {
                ResetEvent(g_console_event);
                LeaveCriticalSection(&g_console_cs);
                break;
            }
            s = g_console_queue[g_console_read];
            g_console_read = (g_console_read + 1) % EEG_CONSOLE_QUEUE;
            LeaveCriticalSection(&g_console_cs);

            if (!base) base = s.tick_ms;
            printf("[%6u ms] CH1=%5u  CH2=%5u\n",
                   (unsigned)(s.tick_ms - base),
                   (unsigned)s.ch1, (unsigned)s.ch2);
        }
        fflush(stdout);
    }
    return 0;
}

static void eeg_console_init(void) {
    if (g_console_ready)
        return;
    InitializeCriticalSection(&g_console_cs);
    g_console_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_console_event) {
        DeleteCriticalSection(&g_console_cs);
        return;
    }
    g_console_shutdown = 0;
    g_console_thread = CreateThread(NULL, 0, EegConsoleThread, NULL, 0, NULL);
    if (!g_console_thread) {
        CloseHandle(g_console_event);
        g_console_event = NULL;
        DeleteCriticalSection(&g_console_cs);
        return;
    }
    g_console_ready = 1;
}

static void eeg_console_shutdown(void) {
    if (!g_console_ready)
        return;
    InterlockedExchange(&g_console_shutdown, 1);
    SetEvent(g_console_event);
    WaitForSingleObject(g_console_thread, 2000);
    CloseHandle(g_console_thread);
    CloseHandle(g_console_event);
    DeleteCriticalSection(&g_console_cs);
    g_console_thread = NULL;
    g_console_event = NULL;
    g_console_ready = 0;
}

static void eeg_console_push(uint16_t ch1, uint16_t ch2) {
    int next;
    if (!g_console_ready)
        return;
    EnterCriticalSection(&g_console_cs);
    next = (g_console_write + 1) % EEG_CONSOLE_QUEUE;
    if (next != g_console_read) {
        g_console_queue[g_console_write].ch1     = ch1;
        g_console_queue[g_console_write].ch2     = ch2;
        g_console_queue[g_console_write].tick_ms = GetTickCount();
        g_console_write = next;
        SetEvent(g_console_event);
    }
    LeaveCriticalSection(&g_console_cs);
}

static void ob_reset_and_start_streaming(void) {
    DWORD count;
    char cmd;
    int i;
    static const char channelCmds[8] = { '!', '@', '#', '$', '%', '^', '&', '*' };

    cmd = 'v';
    WriteFile(hSerial, &cmd, 1, &count, NULL);
    Sleep(100);

    for (i = 0; i < 8; i++) {
        WriteFile(hSerial, &channelCmds[i], 1, &count, NULL);
        WriteFile(hSerial, "5", 1, &count, NULL);
        Sleep(10);
    }

    cmd = 'b';
    WriteFile(hSerial, &cmd, 1, &count, NULL);
    printf("OpenBCI streaming started (%d Hz, 5 s window).\n", OB_SAMPLE_RATE);
}

static int serial_read_byte_or_exit(unsigned char* out) {
    DWORD n = 0;
    for (;;) {
        if (InterlockedCompareExchange(&g_serial_reader_stop, 0, 0))
            return 0;
        if (!ReadFile(hSerial, out, 1, &n, NULL)) {
            DWORD err = GetLastError();
            if (InterlockedCompareExchange(&g_serial_reader_stop, 0, 0))
                return 0;
            if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE)
                return 0;
            continue;
        }
        if (n == 0) {
            Sleep(1);
            continue;
        }
        if (n == 1) return 1;
    }
}

/* Parse all complete EEG-SMT packets out of the stream buffer, update display. */
static void eeg_process_stream(HWND hwndPlot, unsigned char* stream,
                               size_t* stream_len, int* last_counter) {
    /* Skip to first sync header. */
    if (*stream_len >= 2) {
        size_t i;
        for (i = 0; i + 1 < *stream_len; ++i) {
            if (stream[i] == 0xA5 && stream[i + 1] == 0x5A)
                break;
        }
        if (i > 0) {
            memmove(stream, stream + i, *stream_len - i);
            *stream_len -= i;
        }
    }

    while (*stream_len >= 17) {
        if (stream[0] != 0xA5 || stream[1] != 0x5A) {
            memmove(stream, stream + 1, *stream_len - 1);
            *stream_len -= 1;
            continue;
        }

        int counter = stream[3];
        if (*last_counter != -1 && ((counter - *last_counter) & 0xFF) != 1) {
            /* Desync or device reset. */
        }
        *last_counter = counter;

        uint16_t ch1 = (uint16_t)((stream[4] << 8) | stream[5]);
        uint16_t ch2 = (uint16_t)((stream[6] << 8) | stream[7]);

        data[data_index][0] = ch1;
        data[data_index][1] = ch2;
        data_index = (data_index + 1) % MAX_POINTS;

        push_sample(ch1, ch2);
        eeg_console_push(ch1, ch2);

        if (hwndPlot && IsWindow(hwndPlot)) {
            if (InterlockedCompareExchange(&g_eeg_redraw_posted, 1, 0) == 0) {
                if (!PostMessageA(hwndPlot, WM_APP_EEG_SAMPLE, 0, 0))
                    InterlockedExchange(&g_eeg_redraw_posted, 0);
            }
        }

        memmove(stream, stream + 17, *stream_len - 17);
        *stream_len -= 17;
    }
}

DWORD WINAPI SerialThread(LPVOID lpParam) {
    HWND hwndPlot = (HWND)lpParam;
    int last_counter = -1;
    unsigned char readbuf[512];
    unsigned char stream[4096];
    size_t stream_len = 0;
    DWORD bytesRead;
    OVERLAPPED ov;

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 1;

    for (;;) {
        DWORD err;
        BOOL ok;

        if (InterlockedCompareExchange(&g_serial_reader_stop, 0, 0))
            break;

        bytesRead = 0;
        ResetEvent(ov.hEvent);

        /* Issue an overlapped read.  With ReadIntervalTimeout=1 ms, the
         * kernel completes this the moment any byte arrives AND 1 ms passes
         * without another byte — i.e. after each EEG-SMT packet boundary.
         * CancelIoEx (called by stop_serial_io) aborts this instantly. */
        ok = ReadFile(hSerial, readbuf, sizeof(readbuf), NULL, &ov);
        if (!ok) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Block until data arrives, timeout fires, or IO is cancelled. */
                WaitForSingleObject(ov.hEvent, INFINITE);
                if (!GetOverlappedResult(hSerial, &ov, &bytesRead, FALSE)) {
                    err = GetLastError();
                    if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE)
                        break;
                    bytesRead = 0;
                }
            } else if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE) {
                break;
            } else {
                Sleep(5);
                continue;
            }
        } else {
            /* Synchronous completion (data was already buffered). */
            GetOverlappedResult(hSerial, &ov, &bytesRead, FALSE);
        }

        if (InterlockedCompareExchange(&g_serial_reader_stop, 0, 0))
            break;

        if (bytesRead == 0)
            continue;

        if (stream_len + bytesRead > sizeof(stream)) {
            size_t drop = stream_len + bytesRead - sizeof(stream);
            if (drop > stream_len) drop = stream_len;
            memmove(stream, stream + drop, stream_len - drop);
            stream_len -= drop;
        }
        memcpy(stream + stream_len, readbuf, bytesRead);
        stream_len += bytesRead;

        eeg_process_stream(hwndPlot, stream, &stream_len, &last_counter);
    }

    /* Drain any pending overlapped IO before exiting. */
    CancelIoEx(hSerial, &ov);
    GetOverlappedResult(hSerial, &ov, &bytesRead, TRUE);
    CloseHandle(ov.hEvent);
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
    complex float X_conj[MAX_POINTS];
    for (int i = 0; i < N; i++) X_conj[i] = conjf(X[i]);

    complex float x_temp[MAX_POINTS];
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
        x[i] = in[i][ch] - 512.0f;
    }

    fft_cooley_tukey(x, X, N);

    float freq_res = (float)EEG_SAMPLE_RATE / N;
    for (int k = 0; k < N; ++k) {
        float freq = (k <= N / 2) ? k * freq_res : (k - N) * freq_res;
        if (freq >= low_hz && freq <= high_hz) {
            Y[k] = X[k];
        }
    }

    ifft_cooley_tukey(Y, y, N);

    for (int i = 0; i < N; ++i) {
        out[i][ch] = crealf(y[i]) + 512.0f;
    }
}

DWORD WINAPI FilteringThread(LPVOID lpParam) {
    float local_data[MAX_POINTS][2];
    float temp_filtered[MAX_POINTS][2];
    float low_hz = 0.0f, high_hz = 128.0f;

    (void)lpParam;

    while (1) {
        if (current_band == BAND_ALL) {
            Sleep(50);
            continue;
        }

        get_band_limits(current_band, &low_hz, &high_hz);

        EnterCriticalSection(&cs_filtered_data);
        int base_index = (data_index + MAX_POINTS - 1) % MAX_POINTS;
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

        Sleep(1);
    }
    return 0;
}

void draw_plot(HDC hdc, RECT* rect) {
    int titleTop = 26;
    int draw_data_index = data_index;
    if (rect->bottom - rect->top < 100)
        titleTop = 0;

    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, rect, brush);
    DeleteObject(brush);

    const int leftMargin = 60;
    int width = rect->right;
    int height = rect->bottom - rect->top - titleTop;
    int plotWidth = width - leftMargin;

    const int channelPadding = 10;
    const int channelHeight = (height - channelPadding) / 2;

    COLORREF colors[2] = { RGB(0, 128, 255), RGB(255, 0, 0) };

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));
    if (titleTop) {
        const char* t = "Olimex EEG-SMT - 2 channels";
        SetTextColor(hdc, RGB(45, 45, 45));
        TextOutA(hdc, leftMargin, rect->top + 6, t, (int)strlen(t));
    }

    for (int ch = 0; ch < 2; ++ch) {
        float roundedMin = 0.0f;
        float roundedMax = 1024.0f;
        float displayRange = roundedMax - roundedMin;

        int yBase = rect->top + titleTop + ch * (channelHeight + channelPadding);

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
            swprintf(label, 32, L"%.0f", (double)(1024 - value));
            SetTextColor(hdc, RGB(64, 64, 64));
            TextOutW(hdc, 5, textY - 8, label, (int)wcslen(label));
        }

        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        HPEN pen = CreatePen(PS_SOLID, 1, colors[ch]);
        oldPen = (HPEN)SelectObject(hdc, pen);

        if (current_band == BAND_ALL) {
            for (int i = 0; i < MAX_POINTS - 1; ++i) {
                int idx1 = (draw_data_index + i) % MAX_POINTS;
                int idx2 = (draw_data_index + i + 1) % MAX_POINTS;
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

/* ---- OpenBCI drawing & serial parse ---- */

static int ob_mapY_i32(int32_t v, int32_t mn, int32_t mx,
                       int plotTop, int plotBottom, int plotHeight) {
    double t;
    int y;
    if (mx == mn) mx = mn + 1;
    t = (double)(v - mn) / (double)(mx - mn);
    y = plotBottom - (int)(t * plotHeight);
    if (y < plotTop) y = plotTop;
    if (y > plotBottom) y = plotBottom;
    return y;
}

static int ob_find_packet_start(const uint8_t* buf, int len) {
    int i;
    for (i = 0; i <= len - OB_PACKET_SIZE; ++i) {
        if (buf[i] == 0xA0 && buf[i + 32] == 0xC0) return i;
    }
    return -1;
}

static void ob_push_samples_to_history(const int32_t* chans) {
    int c;
    for (c = 0; c < OB_MAX_CHANNELS; ++c) {
        ob_hist[c][ob_writePos] = chans[c];
    }
    ob_writePos = (ob_writePos + 1) % OB_HIST_SAMPLES;
    if (ob_histFilled < OB_HIST_SAMPLES) ob_histFilled++;
}

static void ob_read_serial_data(void) {
    DWORD bytesRead = 0;
    int start;

    if (!ReadFile(hSerial, ob_serialBuffer + ob_serialBufferLength,
                  sizeof(ob_serialBuffer) - ob_serialBufferLength, &bytesRead, NULL)) {
        return;
    }
    ob_serialBufferLength += (int)bytesRead;

    while (ob_serialBufferLength >= OB_PACKET_SIZE &&
           (start = ob_find_packet_start(ob_serialBuffer, ob_serialBufferLength)) != -1) {

        if (start > 0) {
            memmove(ob_serialBuffer, ob_serialBuffer + start, ob_serialBufferLength - start);
            ob_serialBufferLength -= start;
        }

        if (ob_serialBufferLength < OB_PACKET_SIZE) break;

        {
            const int offset = 2;
            int i;
            for (i = 0; i < OB_MAX_CHANNELS; ++i) {
                int idx = offset + i * 3;
                uint8_t b1 = ob_serialBuffer[idx + 0];
                uint8_t b2 = ob_serialBuffer[idx + 1];
                uint8_t b3 = ob_serialBuffer[idx + 2];

                int32_t val = ((int32_t)b1 << 16) | ((int32_t)b2 << 8) | (int32_t)b3;
                if (val & 0x00800000) val |= 0xFF000000;
                ob_lastSample[i] = val;
            }
        }

        ob_push_samples_to_history(ob_lastSample);
        if (is_recording())
            push_openbci_channels(ob_lastSample);

        {
            static unsigned ob_console_skip;
            /* ~10 lines/s at 250 Hz: avoid flooding the console */
            if (++ob_console_skip >= 25) {
                ob_console_skip = 0;
                printf("CH1=%d CH2=%d CH3=%d CH4=%d CH5=%d CH6=%d CH7=%d CH8=%d\n",
                    (int)ob_lastSample[0], (int)ob_lastSample[1], (int)ob_lastSample[2],
                    (int)ob_lastSample[3], (int)ob_lastSample[4], (int)ob_lastSample[5],
                    (int)ob_lastSample[6], (int)ob_lastSample[7]);
                fflush(stdout);
            }
        }

        memmove(ob_serialBuffer, ob_serialBuffer + OB_PACKET_SIZE, ob_serialBufferLength - OB_PACKET_SIZE);
        ob_serialBufferLength -= OB_PACKET_SIZE;
    }

    if (ob_serialBufferLength > (int)sizeof(ob_serialBuffer) - 128) {
        ob_serialBufferLength = 0;
    }

    ob_update_band_filter_cache();
}

static void ob_compute_band_for_channel(int c) {
    float low_hz, high_hz;
    int N = MAX_POINTS;
    int i, k;
    complex float x[MAX_POINTS];
    complex float X[MAX_POINTS] = { 0 };
    complex float Y[MAX_POINTS] = { 0 };
    complex float y[MAX_POINTS];
    float mean = 0.0f;

    get_band_limits(current_band, &low_hz, &high_hz);
    for (i = 0; i < N; i++) {
        int idx = (ob_writePos - N + i + OB_HIST_SAMPLES) % OB_HIST_SAMPLES;
        x[i] = (float)ob_hist[c][idx];
        mean += x[i];
    }
    mean /= (float)N;
    for (i = 0; i < N; i++) x[i] -= mean;

    fft_cooley_tukey(x, X, N);
    {
        float freq_res = (float)OB_SAMPLE_RATE / (float)N;
        for (k = 0; k < N; ++k) {
            float freq = (k <= N / 2) ? (float)k * freq_res : (float)(k - N) * freq_res;
            if (freq >= low_hz && freq <= high_hz)
                Y[k] = X[k];
        }
    }
    ifft_cooley_tukey(Y, y, N);
    for (i = 0; i < N; i++)
        ob_band_line[c][i] = (int32_t)(crealf(y[i]) + mean);
}

static void ob_update_band_filter_cache(void) {
    int c;
    static DWORD lastTick;
    if (current_band == BAND_ALL || ob_histFilled < MAX_POINTS) {
        ob_band_cache_ok = 0;
        return;
    }
    if (GetTickCount() - lastTick < 80u)
        return;
    lastTick = GetTickCount();
    for (c = 0; c < OB_MAX_CHANNELS; c++)
        ob_compute_band_for_channel(c);
    ob_band_cache_ok = 1;
}

static void ob_draw_strip(HDC mdc, RECT r, int chanIdx) {
    HBRUSH bg;
    HPEN border;
    HGDIOBJ oldPen;
    int32_t mn, mx;
    int i;
    int h, w, midY;
    HPEN grid;
    char lbl[32];
    HPEN sig;
    int leftPad, plotLeft, plotRight, plotWidth, plotTop, plotBottom, plotHeight;
    int samples;
    double step;
    int xpix;

    bg = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(mdc, &r, bg);
    DeleteObject(bg);

    border = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    oldPen = SelectObject(mdc, border);
    Rectangle(mdc, r.left, r.top, r.right, r.bottom);
    SelectObject(mdc, oldPen);
    DeleteObject(border);

    if (ob_histFilled == 0) return;

    mn = INT32_MAX; mx = INT32_MIN;
    if (current_band != BAND_ALL && ob_band_cache_ok && ob_histFilled >= MAX_POINTS) {
        for (i = 0; i < MAX_POINTS; ++i) {
            int32_t v = ob_band_line[chanIdx][i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    } else {
        for (i = 0; i < ob_histFilled; ++i) {
            int idx = (ob_writePos + i) % OB_HIST_SAMPLES;
            int32_t v = ob_hist[chanIdx][idx];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }
    if (mn == mx) { mn -= 1; mx += 1; }

    h = r.bottom - r.top;
    w = r.right - r.left;
    midY = r.top + h / 2;
    grid = CreatePen(PS_DOT, 1, RGB(80, 80, 80));
    oldPen = SelectObject(mdc, grid);
    MoveToEx(mdc, r.left + 40, midY, NULL);
    LineTo(mdc, r.right - 5, midY);
    SelectObject(mdc, oldPen);
    DeleteObject(grid);

    sprintf(lbl, "CH%d", chanIdx + 1);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(200, 200, 200));
    TextOutA(mdc, r.left + 8, r.top + 6, lbl, (int)strlen(lbl));

    sig = CreatePen(PS_SOLID, 1, RGB(0, 200, 255));
    oldPen = SelectObject(mdc, sig);

    leftPad = 40;
    plotLeft   = r.left + leftPad;
    plotRight  = r.right - 8;
    plotWidth  = MAXI(1, plotRight - plotLeft);
    plotTop    = r.top + 4;
    plotBottom = r.bottom - 4;
    plotHeight = MAXI(1, plotBottom - plotTop);

    samples = ob_histFilled;
    if (current_band != BAND_ALL && ob_band_cache_ok && ob_histFilled >= MAX_POINTS)
        samples = MAX_POINTS;
    /* Always spread all available samples across the full plot width so the
     * waveform fills the window regardless of its size. */
    step = (double)samples / (double)plotWidth;

    for (xpix = 0; xpix < plotWidth; ++xpix) {
        int sampleIndexFromOldest = (int)floor(xpix * step);
        int32_t v;
        int x, y;

        if (sampleIndexFromOldest >= samples) sampleIndexFromOldest = samples - 1;
        if (current_band != BAND_ALL && ob_band_cache_ok && ob_histFilled >= MAX_POINTS)
            v = ob_band_line[chanIdx][sampleIndexFromOldest];
        else {
            int ringIdx = (ob_writePos + sampleIndexFromOldest) % OB_HIST_SAMPLES;
            v = ob_hist[chanIdx][ringIdx];
        }

        x = plotLeft + xpix;
        y = ob_mapY_i32(v, mn, mx, plotTop, plotBottom, plotHeight);

        if (xpix == 0)
            MoveToEx(mdc, x, y, NULL);
        else
            LineTo(mdc, x, y);
    }

    SelectObject(mdc, oldPen);
    DeleteObject(sig);
}

static void ob_draw_eeg(HDC hdc, RECT client) {
    const int titleH = 26;
    HDC mdc;
    HBITMAP mbmp;
    HGDIOBJ oldBmp;
    HBRUSH bg;
    int totalH, stripH;
    int c;
    RECT r;
    RECT plotArea;

    mdc = CreateCompatibleDC(hdc);
    mbmp = CreateCompatibleBitmap(hdc, client.right - client.left, client.bottom - client.top);
    oldBmp = SelectObject(mdc, mbmp);

    bg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(mdc, &client, bg);
    DeleteObject(bg);

    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(200, 200, 200));
    {
        const char* t = "OpenBCI Cyton Board - 8 channels";
        TextOutA(mdc, client.left + 12, client.top + 4, t, (int)strlen(t));
    }

    plotArea = client;
    plotArea.top = client.top + titleH;
    totalH = plotArea.bottom - plotArea.top;
    stripH = MAXI(1, totalH / OB_MAX_CHANNELS);

    for (c = 0; c < OB_MAX_CHANNELS; ++c) {
        r.left   = plotArea.left + 8;
        r.right  = plotArea.right - 8;
        r.top    = plotArea.top + c * stripH + 2;
        r.bottom = (c == OB_MAX_CHANNELS - 1) ? plotArea.bottom - 6 : plotArea.top + (c + 1) * stripH - 2;
        ob_draw_strip(mdc, r, c);
    }

    BitBlt(hdc, client.left, client.top, client.right - client.left, client.bottom - client.top,
           mdc, 0, 0, SRCCOPY);

    SelectObject(mdc, oldBmp);
    DeleteObject(mbmp);
    DeleteDC(mdc);
}

static void build_menus(HWND hwnd) {
    HMENU hOld = GetMenu(hwnd);
    /* Load the menu structure from the compiled resource (IDR_MAINMENU in resources.rc).
     * The only runtime customisation needed is ticking the active frequency band. */
    HMENU hMenuBar = LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_MAINMENU));
    if (!hMenuBar) return;

    for (int i = ID_BAND_ALL; i <= ID_BAND_GAMMA; ++i)
        CheckMenuItem(hMenuBar, i, MF_BYCOMMAND |
            ((i == (ID_BAND_ALL + current_band)) ? MF_CHECKED : MF_UNCHECKED));

    SetMenu(hwnd, hMenuBar);
    if (hOld) DestroyMenu(hOld);
}

static void stop_serial_io(HWND hwnd) {
    if (!g_io_started)
        return;

    if (g_device == DEVICE_OPENBCI) {
        KillTimer(hwnd, IDT_OPENBCI_TIMER);
    } else {
        InterlockedExchange(&g_eeg_redraw_posted, 0);
        InterlockedExchange(&g_serial_reader_stop, 1);
        if (hSerial != INVALID_HANDLE_VALUE)
            CancelIoEx(hSerial, NULL);
        if (g_serial_thread_handle) {
            WaitForSingleObject(g_serial_thread_handle, 8000);
            CloseHandle(g_serial_thread_handle);
            g_serial_thread_handle = NULL;
        }
        InterlockedExchange(&g_serial_reader_stop, 0);
    }

    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }

    if (g_device == DEVICE_OPENBCI) {
        ob_serialBufferLength = 0;
        ob_writePos = 0;
        ob_histFilled = 0;
        memset(ob_hist, 0, sizeof(ob_hist));
    }
}

/* After stop_serial_io: open g_com_port and start reader/timer. */
static int restore_serial_after_stop(HWND hwnd, const char* device_path) {
    strncpy(g_com_port, device_path, sizeof(g_com_port));
    g_com_port[sizeof(g_com_port) - 1] = 0;

    if (g_device == DEVICE_EEG_SMT) {
        if (!g_eeg_filter_ready) {
            InitializeCriticalSection(&cs_filtered_data);
            CreateThread(NULL, 0, FilteringThread, NULL, 0, NULL);
            g_eeg_filter_ready = 1;
        }
        if (!init_serial_eeg_smt(g_com_port))
            return 0;
        g_serial_thread_handle = CreateThread(NULL, 0, SerialThread, hwnd, 0, NULL);
        if (!g_serial_thread_handle) {
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            return 0;
        }
        return 1;
    }

    if (!init_serial_openbci(g_com_port))
        return 0;
    ob_reset_and_start_streaming();
    SetTimer(hwnd, IDT_OPENBCI_TIMER, 100, NULL);
    return 1;
}

static void enter_disconnected_state(HWND hwnd) {
    g_io_started = 0;
    snprintf(g_connection_status, sizeof(g_connection_status),
        "No EEG device was detected. Check USB/power, drivers; use File - Settings for COM.");
    SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);
    SetWindowTextA(hwnd, "Local Neural Monitoring - starting...");
    build_menus(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
}

static int reconnect_serial(HWND hwnd) {
    char resolved[SETTINGS_COM_NAME_LEN];
    char title[256];
    char saved_port[64];
    char port_after_settings[64];
    DeviceKind saved_device;

    if (g_cmdline_overrode_port)
        return 1;

    strncpy(saved_port, g_com_port, sizeof(saved_port));
    saved_port[sizeof(saved_port) - 1] = 0;
    saved_device = g_device;

    stop_serial_io(hwnd);
    g_io_started = 0;

    settings_resolve_port(resolved, sizeof(resolved), 0);
    com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
    strncpy(port_after_settings, g_com_port, sizeof(port_after_settings));
    port_after_settings[sizeof(port_after_settings) - 1] = 0;

    {
        HardwareKind hk = HW_NONE_DETECTED;
        if (hw_detect_probe_port_reconnect(g_com_port, &hk)) {
            g_device = (hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
            if (restore_serial_after_stop(hwnd, g_com_port)) {
                g_io_started = 1;
                if (g_device == DEVICE_OPENBCI)
                    SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
                else
                    SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);
                build_menus(hwnd);
                snprintf(g_connection_status, sizeof(g_connection_status), "Reconnected on %s",
                    hw_detect_port_display_name(g_com_port));
                snprintf(title, sizeof(title), "Local Neural Monitoring - %s - %s",
                    g_device == DEVICE_OPENBCI ? "OpenBCI (8 ch)" : "EEG-SMT (2 ch)",
                    hw_detect_port_display_name(g_com_port));
                SetWindowTextA(hwnd, title);
                InvalidateRect(hwnd, NULL, FALSE);
                return 1;
            }
        }
    }

    /* New port: no hardware signature or could not open — restore previous session. */
    strncpy(g_com_port, saved_port, sizeof(g_com_port));
    g_com_port[sizeof(g_com_port) - 1] = 0;
    g_device = saved_device;

    {
        HardwareKind hk = HW_NONE_DETECTED;
        if (hw_detect_probe_port_reconnect(g_com_port, &hk))
            g_device = (hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
        /* If quick probe misses a slow board, keep saved_device and still try to reopen. */
        if (restore_serial_after_stop(hwnd, g_com_port)) {
            g_io_started = 1;
            if (g_device == DEVICE_OPENBCI)
                SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
            else
                SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);
            build_menus(hwnd);
            snprintf(g_connection_status, sizeof(g_connection_status),
                _stricmp(saved_port, port_after_settings) == 0
                    ? "Reconnected on %s"
                    : "No EEG device on chosen port - still on %s",
                hw_detect_port_display_name(g_com_port));
            snprintf(title, sizeof(title), "Local Neural Monitoring - %s - %s",
                g_device == DEVICE_OPENBCI ? "OpenBCI (8 ch)" : "EEG-SMT (2 ch)",
                hw_detect_port_display_name(g_com_port));
            SetWindowTextA(hwnd, title);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
    }

    enter_disconnected_state(hwnd);
    return 0;
}

static int application_open_serial_and_start_io(HWND hwnd) {
    char title[256];

    if (g_io_started) return 1;

    if (g_device == DEVICE_OPENBCI) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        printf("%s\n", g_connection_status);
        printf("OpenBCI mode (8 ch).\n");
    }

    if (g_device == DEVICE_EEG_SMT) {
        if (!init_serial_eeg_smt(g_com_port)) {
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Could not open %s", hw_detect_port_display_name(g_com_port));
            build_menus(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
    } else {
        if (!init_serial_openbci(g_com_port)) {
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Could not open %s", hw_detect_port_display_name(g_com_port));
            build_menus(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        ob_reset_and_start_streaming();
    }

    build_menus(hwnd);

    if (g_device == DEVICE_EEG_SMT) {
        if (!g_eeg_filter_ready) {
            InitializeCriticalSection(&cs_filtered_data);
            CreateThread(NULL, 0, FilteringThread, NULL, 0, NULL);
            g_eeg_filter_ready = 1;
        }
        eeg_console_init();
    }

    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    SetConsoleOutputCP(CP_UTF8);

    if (g_device == DEVICE_EEG_SMT) {
        InterlockedExchange(&g_eeg_redraw_posted, 0);
        g_serial_thread_handle = CreateThread(NULL, 0, SerialThread, hwnd, 0, NULL);
        if (!g_serial_thread_handle) {
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Could not start serial reader thread");
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            if (g_eeg_filter_ready) {
                DeleteCriticalSection(&cs_filtered_data);
                g_eeg_filter_ready = 0;
            }
            build_menus(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
    } else {
        SetTimer(hwnd, IDT_OPENBCI_TIMER, 100, NULL);
    }

    snprintf(title, sizeof(title), "Local Neural Monitoring - %s - %s",
        g_device == DEVICE_OPENBCI ? "OpenBCI (8 ch)" : "EEG-SMT (2 ch)",
        hw_detect_port_display_name(g_com_port));
    SetWindowTextA(hwnd, title);

    InvalidateRect(hwnd, NULL, FALSE);
    g_io_started = 1;
    return 1;
}

/* First connection from File->Settings OK before background scan has opened serial. */
static void apply_saved_settings_probe_and_open(HWND hwnd) {
    char resolved[SETTINGS_COM_NAME_LEN];
    HardwareKind hk = HW_NONE_DETECTED;

    settings_resolve_port(resolved, sizeof(resolved), 0);
    com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));

    if (hw_detect_probe_port(g_com_port, &hk)) {
        g_device = (hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
        snprintf(g_connection_status, sizeof(g_connection_status),
            "Connected: %s on %s (settings)",
            hk == HW_OPENBCI ? "OpenBCI Cyton Board" : "Olimex EEG-SMT",
            hw_detect_port_display_name(g_com_port));
    } else {
        snprintf(g_connection_status, sizeof(g_connection_status),
            "Opening %s (hardware not verified on probe)",
            hw_detect_port_display_name(g_com_port));
    }

    if (g_device == DEVICE_OPENBCI)
        SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
    else
        SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);

    (void)application_open_serial_and_start_io(hwnd);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

/* After settings_resolve + com_name_to_device_path: set g_device from probe and match window size. */
static void probe_saved_port_resize_window(HWND hwnd) {
    HardwareKind hk = HW_NONE_DETECTED;
    if (hw_detect_probe_port(g_com_port, &hk))
        g_device = (hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
    if (g_device == DEVICE_OPENBCI)
        SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
    else
        SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);
}

static DWORD WINAPI HardwareDetectThread(LPVOID lpParam) {
    HWND hwnd = (HWND)lpParam;
    HardwareKind hk = HW_NONE_DETECTED;
    char dport[64];
    DetectResult* r;
    int attempt;

    r = (DetectResult*)malloc(sizeof(DetectResult));
    if (!r) {
        if (IsWindow(hwnd))
            PostMessageA(hwnd, WM_APP_DETECT_DONE, 0, 0);
        return 0;
    }
    memset(r, 0, sizeof(*r));

    if (IsWindow(hwnd))
        PostMessageA(hwnd, WM_APP_SCAN_PROGRESS, 0, 0);

    /* Prefer Windows device metadata (friendly name / USB UART id) before slow per-port banner scan. */
    {
        int trust_skip_banner = 0;
        if (hw_detect_com_openbci_from_system(dport, sizeof(dport), &trust_skip_banner) &&
            (trust_skip_banner || hw_detect_try_openbci_quick(dport))) {
            r->found = 1;
            r->hk = HW_OPENBCI;
            strncpy(r->port, dport, sizeof(r->port) - 1);
            r->port[sizeof(r->port) - 1] = 0;
            if (!IsWindow(hwnd)) {
                free(r);
                return 0;
            }
            PostMessageA(hwnd, WM_APP_DETECT_DONE, 0, (LPARAM)r);
            return 0;
        }
    }

    /* Autodetect: hw_detect_scan() — all COM ports, OpenBCI first then EEG-SMT (see hw_detect.c). */
    for (attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0)
            Sleep(400);
        r->found = hw_detect_scan(dport, sizeof(dport), &hk);
        if (r->found) {
            r->hk = hk;
            strncpy(r->port, dport, sizeof(r->port) - 1);
            r->port[sizeof(r->port) - 1] = 0;
            break;
        }
    }
    if (!IsWindow(hwnd)) {
        free(r);
        return 0;
    }
    PostMessageA(hwnd, WM_APP_DETECT_DONE, 0, (LPARAM)r);
    return 0;
}

static void draw_connection_status_bar(HDC hdc, const RECT* client) {
    RECT r, textR;
    HBRUSH br;
    HPEN pen;
    HGDIOBJ oldPen;
    const char* txt = (g_connection_status[0] != 0) ? g_connection_status : "Ready";

    r = *client;
    r.top = r.bottom - STATUS_BAR_HEIGHT;

    br = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
    FillRect(hdc, &r, br);
    DeleteObject(br);

    pen = CreatePen(PS_SOLID, 1, RGB(172, 172, 178));
    oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, r.left, r.top, NULL);
    LineTo(hdc, r.right, r.top);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    textR = r;
    textR.left += 10;
    textR.right -= 10;
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));
    SetTextColor(hdc, RGB(28, 28, 32));
    DrawTextA(hdc, txt, -1, &textR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* No fake EEG traces before serial is open (async startup or failed open). */
static void draw_startup_placeholder(HDC hdc, const RECT* client) {
    HBRUSH bg = CreateSolidBrush(RGB(252, 252, 252));
    RECT body = *client;
    const char* msg;
    body.bottom -= STATUS_BAR_HEIGHT;
    FillRect(hdc, &body, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(55, 55, 55));
    SelectObject(hdc, GetStockObject(ANSI_VAR_FONT));
    if (strstr(g_connection_status, "No EEG device was detected") != NULL) {
        msg = "No EEG device was detected.\r\n\r\n"
              "Check that the hardware is plugged in, turned on, and that drivers are installed "
              "(Windows Device Manager: Ports (COM & LPT) or Universal Serial Bus).\r\n\r\n"
              "Then use the menu: File - Settings... to choose the correct COM port.";
    } else {
        msg = "No serial link yet (nothing connected or still scanning).\r\n\r\n"
              "Use the menu: File - Settings... to choose a serial port, or wait for auto-detection.";
    }
    DrawTextA(hdc, msg, -1, &body, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    const char CLASS_NAME[] = "EEGWindow";
    WNDCLASS wc = { 0 };
    HWND hwnd;
    char title[256];

    (void)hPrev;

    /* Elevated helper mode: "--ftdi-lat COM6"
     * Launched by elevate_and_set_ftdi_latency() with "runas" verb.
     * Writes LatencyTimer=1 to the FTDIBUS registry key and exits immediately. */
    if (lpCmdLine && _strnicmp(lpCmdLine, "--ftdi-lat ", 11) == 0) {
        char com_name[32] = {0};
        strncpy(com_name, lpCmdLine + 11, sizeof(com_name) - 1);
        /* strip any trailing whitespace/quotes */
        for (int i = (int)strlen(com_name) - 1;
             i >= 0 && (com_name[i] == ' ' || com_name[i] == '"'); --i)
            com_name[i] = '\0';
        do_set_ftdi_latency_elevated(com_name);
        return 0;
    }

    parse_cmdline(lpCmdLine);

    g_connection_status[0] = 0;

    if (!g_cmdline_empty) {
        if (g_cmdline_overrode_port && !g_cmdline_explicit_openbci) {
            HardwareKind hk = HW_NONE_DETECTED;
            if (hw_detect_probe_port(g_com_port, &hk)) {
                g_device = (hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
                snprintf(g_connection_status, sizeof(g_connection_status),
                    "Detected: %s on %s",
                    hk == HW_OPENBCI ? "OpenBCI Cyton Board" : "Olimex EEG-SMT",
                    hw_detect_port_display_name(g_com_port));
            } else {
                snprintf(g_connection_status, sizeof(g_connection_status),
                    "Could not verify hardware on %s - using EEG-SMT mode",
                    hw_detect_port_display_name(g_com_port));
            }
        } else {
            if (!g_cmdline_overrode_port) {
                char resolved[SETTINGS_COM_NAME_LEN];
                settings_resolve_port(resolved, sizeof(resolved), 0);
                com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
            }
            snprintf(g_connection_status, sizeof(g_connection_status),
                "%s on %s",
                g_device == DEVICE_OPENBCI ? "OpenBCI Cyton Board" : "Olimex EEG-SMT",
                hw_detect_port_display_name(g_com_port));
        }
    } else {
        char resolved[SETTINGS_COM_NAME_LEN];
        settings_resolve_port(resolved, sizeof(resolved), 0);
        com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
        snprintf(g_connection_status, sizeof(g_connection_status),
            "Scanning serial ports for hardware...");
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    if (g_cmdline_empty) {
        snprintf(title, sizeof(title), "Local Neural Monitoring - starting...");
        hwnd = CreateWindowEx(0, CLASS_NAME, title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 800, 400, NULL, NULL, hInst, NULL);
    } else {
        snprintf(title, sizeof(title), "Local Neural Monitoring - %s - %s",
            g_device == DEVICE_OPENBCI ? "OpenBCI (8 ch)" : "EEG-SMT (2 ch)",
            hw_detect_port_display_name(g_com_port));
        hwnd = CreateWindowEx(0, CLASS_NAME, title, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            g_device == DEVICE_OPENBCI ? 1100 : 800,
            g_device == DEVICE_OPENBCI ? 700 : 400,
            NULL, NULL, hInst, NULL);
    }
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    if (g_cmdline_empty)
        InvalidateRect(hwnd, NULL, FALSE);

    CheckAndDisableSerialMouse();

    /* Menu bar immediately (not deferred until serial opens). Rebuilt after connect if device type changes. */
    build_menus(hwnd);

    if (g_cmdline_empty) {
        if (!CreateThread(NULL, 0, HardwareDetectThread, hwnd, 0, NULL)) {
            MessageBoxA(hwnd, "Could not start hardware detection thread. Using saved COM settings.", "Error", MB_OK | MB_ICONERROR);
            {
                char resolved[SETTINGS_COM_NAME_LEN];
                settings_resolve_port(resolved, sizeof(resolved), 0);
                com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
                probe_saved_port_resize_window(hwnd);
                snprintf(g_connection_status, sizeof(g_connection_status),
                    "Thread start failed - opening %s (%s)",
                    hw_detect_port_display_name(g_com_port),
                    g_device == DEVICE_OPENBCI ? "OpenBCI" : "EEG-SMT");
            }
            if (!application_open_serial_and_start_io(hwnd))
                return 0;
        }
    } else {
        if (!application_open_serial_and_start_io(hwnd))
            return 0;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hSerial != INVALID_HANDLE_VALUE)
        CloseHandle(hSerial);
    if (g_eeg_filter_ready) {
        DeleteCriticalSection(&cs_filtered_data);
        g_eeg_filter_ready = 0;
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_EEG_SAMPLE:
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        return 0;
    case WM_APP_SCAN_PROGRESS:
        snprintf(g_connection_status, sizeof(g_connection_status),
            "Scanning all COM ports (autodetect)...");
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        return 0;
    case WM_APP_DETECT_DONE: {
        DetectResult* r = (DetectResult*)lParam;
        if (!g_cmdline_empty) {
            if (r) free(r);
            return 0;
        }
        if (g_io_started) {
            if (r) free(r);
            return 0;
        }
        if (!r) {
            char resolved[SETTINGS_COM_NAME_LEN];
            settings_resolve_port(resolved, sizeof(resolved), 0);
            com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
            probe_saved_port_resize_window(hwnd);
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Detection unavailable — opening %s (%s)",
                hw_detect_port_display_name(g_com_port),
                g_device == DEVICE_OPENBCI ? "OpenBCI" : "EEG-SMT");
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);
            (void)application_open_serial_and_start_io(hwnd);
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            return 0;
        }
        if (r->found) {
            strncpy(g_com_port, r->port, sizeof(g_com_port) - 1);
            g_com_port[sizeof(g_com_port) - 1] = 0;
            g_device = (r->hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Connected: %s on %s (auto-detected)",
                r->hk == HW_OPENBCI ? "OpenBCI Cyton Board" : "Olimex EEG-SMT",
                hw_detect_port_display_name(g_com_port));
            free(r);
            if (g_device == DEVICE_OPENBCI) {
                SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
            }
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);
            (void)application_open_serial_and_start_io(hwnd);
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            return 0;
        }
        {
            char resolved[SETTINGS_COM_NAME_LEN];
            settings_resolve_port(resolved, sizeof(resolved), 0);
            com_name_to_device_path(resolved, g_com_port, sizeof(g_com_port));
            probe_saved_port_resize_window(hwnd);
            snprintf(g_connection_status, sizeof(g_connection_status),
                "Full scan found nothing — opening saved port %s (%s)",
                hw_detect_port_display_name(g_com_port),
                g_device == DEVICE_OPENBCI ? "OpenBCI" : "EEG-SMT");
        }
        free(r);
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateWindow(hwnd);
        (void)application_open_serial_and_start_io(hwnd);
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return 0;
    }
    case WM_APP_SETTINGS_PAUSE_SERIAL:
        if (g_io_started) {
            strncpy(g_saved_com_port, g_com_port, sizeof(g_saved_com_port));
            g_saved_com_port[sizeof(g_saved_com_port) - 1] = 0;
            g_saved_device_kind = g_device;
            g_settings_detect_had_serial = 1;
            stop_serial_io(hwnd);
            g_io_started = 0;
        } else {
            g_settings_detect_had_serial = 0;
        }
        return 0;
    case WM_APP_SETTINGS_RESUME_SERIAL: {
        SettingsDetectResult* r = (SettingsDetectResult*)lParam;
        int found = r && r->found;

        if (found) {
            strncpy(g_com_port, r->path, sizeof(g_com_port));
            g_com_port[sizeof(g_com_port) - 1] = 0;
            g_device = (r->hk == HW_OPENBCI) ? DEVICE_OPENBCI : DEVICE_EEG_SMT;
            if (g_device == DEVICE_OPENBCI)
                SetWindowPos(hwnd, NULL, 0, 0, 1100, 700, SWP_NOMOVE | SWP_NOZORDER);
            else
                SetWindowPos(hwnd, NULL, 0, 0, 800, 400, SWP_NOMOVE | SWP_NOZORDER);
        } else if (g_settings_detect_had_serial) {
            strncpy(g_com_port, g_saved_com_port, sizeof(g_com_port));
            g_com_port[sizeof(g_com_port) - 1] = 0;
            g_device = g_saved_device_kind;
        }

        if (r)
            free(r);

        if (found || g_settings_detect_had_serial) {
            if (!application_open_serial_and_start_io(hwnd)) {
                snprintf(g_connection_status, sizeof(g_connection_status),
                    "Could not reopen %s after detection", hw_detect_port_display_name(g_com_port));
            } else {
                snprintf(g_connection_status, sizeof(g_connection_status),
                    "Connected: %s on %s",
                    g_device == DEVICE_OPENBCI ? "OpenBCI Cyton Board" : "Olimex EEG-SMT",
                    hw_detect_port_display_name(g_com_port));
            }
            build_menus(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        g_settings_detect_had_serial = 0;
        return 0;
    }
    case WM_TIMER:
        if (g_io_started && g_device == DEVICE_OPENBCI && wParam == IDT_OPENBCI_TIMER) {
            ob_read_serial_data();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_SIZE:
        if (g_io_started)
            InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        if (!g_io_started) {
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            HGDIOBJ oldBM = SelectObject(memDC, memBM);
            {
                HBRUSH bgfull = CreateSolidBrush(RGB(252, 252, 252));
                FillRect(memDC, &rect, bgfull);
                DeleteObject(bgfull);
            }
            draw_startup_placeholder(memDC, &rect);
            draw_connection_status_bar(memDC, &rect);
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
        } else if (g_device == DEVICE_OPENBCI) {
            RECT plotRect = rect;
            plotRect.bottom -= STATUS_BAR_HEIGHT;
            ob_draw_eeg(hdc, plotRect);
            draw_connection_status_bar(hdc, &rect);
        } else {
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            HGDIOBJ oldBM = SelectObject(memDC, memBM);
            {
                RECT plotRect = rect;
                plotRect.bottom -= STATUS_BAR_HEIGHT;
                draw_plot(memDC, &plotRect);
            }
            draw_connection_status_bar(memDC, &rect);
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
        }
        EndPaint(hwnd, &ps);
        InterlockedExchange(&g_eeg_redraw_posted, 0);
        break;
    }
    case WM_DESTROY:
        if (g_io_started)
            stop_serial_io(hwnd);
        eeg_console_shutdown();
        PostQuitMessage(0);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_FILE_EXIT:
            PostQuitMessage(0);
            break;
        case ID_FILE_START_RECORDING:
            if (!g_io_started) break;
            if (g_device != DEVICE_EEG_SMT && g_device != DEVICE_OPENBCI) break;
            if (!is_recording()) {
                init_recording(g_device == DEVICE_OPENBCI ? 8 : 2);
                MessageBoxA(hwnd,
                    g_device == DEVICE_OPENBCI
                        ? "Recording 8 channels into eeg_data.csv has started."
                        : "Recording 2 channels into eeg_data.csv has started.",
                    "Info", MB_OK);
            }
            break;
        case ID_FILE_STOP_RECORDING:
            if (!g_io_started) break;
            if (g_device != DEVICE_EEG_SMT && g_device != DEVICE_OPENBCI) break;
            if (is_recording()) {
                stop_recording();
                {
                    const char *csv_path = recording_last_path();
                    if (csv_path && csv_path[0]) {
                        /* Update status bar. */
                        snprintf(g_connection_status, sizeof(g_connection_status),
                                 "Recording saved: %s", csv_path);
                        InvalidateRect(hwnd, NULL, FALSE);
                        /* Open Explorer with the file pre-selected. */
                        char arg[MAX_PATH + 12];
                        snprintf(arg, sizeof(arg), "/select,\"%s\"", csv_path);
                        ShellExecuteA(hwnd, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
                    }
                }
            }
            break;
        case ID_FILE_SETTINGS: {
            const char* pref_com = NULL;
            char pref_buf[SETTINGS_COM_NAME_LEN];
            if (g_com_port[0]) {
                strncpy(pref_buf, hw_detect_port_display_name(g_com_port), sizeof(pref_buf) - 1);
                pref_buf[sizeof(pref_buf) - 1] = 0;
                if (settings_port_in_enum(pref_buf))
                    pref_com = pref_buf;
            }
            INT_PTR dr = settings_show_dialog(hwnd, (HINSTANCE)(ULONG_PTR)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
                pref_com);
            if (dr != IDOK)
                break;
            if (g_io_started)
                (void)reconnect_serial(hwnd);
            else
                apply_saved_settings_probe_and_open(hwnd);
            break;
        }
        case ID_BAND_ALL:
        case ID_BAND_DELTA:
        case ID_BAND_THETA:
        case ID_BAND_ALPHA:
        case ID_BAND_BETA:
        case ID_BAND_GAMMA:
            if (!g_io_started || (g_device != DEVICE_EEG_SMT && g_device != DEVICE_OPENBCI)) break;
            current_band = (FrequencyBand)(LOWORD(wParam) - ID_BAND_ALL);
            for (int i = ID_BAND_ALL; i <= ID_BAND_GAMMA; ++i) {
                CheckMenuItem(GetMenu(hwnd), i, MF_BYCOMMAND | ((i == LOWORD(wParam)) ? MF_CHECKED : MF_UNCHECKED));
            }
            if (g_device == DEVICE_OPENBCI)
                InvalidateRect(hwnd, NULL, TRUE);
            break;

        case ID_HELP_ABOUT:
            
                MessageBoxW(hwnd,
                    L"Local Neural Monitoring v 0.3.0\n"
                    L"Released under the MIT License.\n"
                    L"Author: Michal Oblastni\n"
                    L"https://github.com/michaloblastni",
                    L"About", MB_OK | MB_ICONINFORMATION);
            
            break;
        case ID_HELP_CONTENTS: {
            char exePath[MAX_PATH];
            char chmPath[MAX_PATH];
            char* slash;
            if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0)
                break;
            slash = strrchr(exePath, '\\');
            if (slash) *slash = 0;
            snprintf(chmPath, sizeof(chmPath), "%s\\help.chm", exePath);
            ShellExecuteA(NULL, "open", chmPath, NULL, exePath, SW_SHOWNORMAL);
            break;
        }
        }
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
