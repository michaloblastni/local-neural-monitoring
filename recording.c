#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>
#include "recording.h"

#define MAX_QUEUE 1024

typedef struct {
    int32_t ch[RECORDING_MAX_CHANNELS];
    int nch;
    uint64_t timestamp;
} RecSample;

static RecSample queue[MAX_QUEUE];
static volatile int write_idx = 0;
static volatile int read_idx = 0;
static HANDLE mutex;
static HANDLE event;
static FILE* csv = NULL;
static volatile BOOL recording = FALSE;
static int g_rec_nch = 0;
static char g_last_csv_path[MAX_PATH];

DWORD WINAPI CsvWriterThread(LPVOID param) {
    (void)param;
    while (1) {
        int i;
        WaitForSingleObject(event, INFINITE);

        while (read_idx != write_idx) {
            RecSample s;
            WaitForSingleObject(mutex, INFINITE);
            s = queue[read_idx];
            read_idx = (read_idx + 1) % MAX_QUEUE;
            ReleaseMutex(mutex);

            if (csv) {
                fprintf(csv, "%llu", (unsigned long long)s.timestamp);
                for (i = 0; i < s.nch; i++)
                    fprintf(csv, ",%d", (int)s.ch[i]);
                fprintf(csv, "\n");
            }
        }
    }
    return 0;
}

void init_recording(int num_channels) {
    int i;
    if (!recording) {
        if (num_channels < 1 || num_channels > RECORDING_MAX_CHANNELS)
            return;
        GetFullPathNameA("eeg_data.csv", sizeof(g_last_csv_path), g_last_csv_path, NULL);
        csv = fopen(g_last_csv_path, "w");
        if (!csv) return;

        g_rec_nch = num_channels;
        fprintf(csv, "timestamp");
        for (i = 0; i < num_channels; i++)
            fprintf(csv, ",ch%d", i + 1);
        fprintf(csv, "\n");

        mutex = CreateMutex(NULL, FALSE, NULL);
        event = CreateEvent(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, CsvWriterThread, NULL, 0, NULL);
        recording = TRUE;
    }
}

void stop_recording(void) {
    if (recording) {
        recording = FALSE;
        Sleep(50);
        fclose(csv);
        csv = NULL;
        g_rec_nch = 0;
    }
}

void push_sample(uint16_t ch1, uint16_t ch2) {
    RecSample s;
    int next;
    if (!recording || g_rec_nch != 2) return;
    s.nch = 2;
    s.ch[0] = (int32_t)ch1;
    s.ch[1] = (int32_t)ch2;
    s.timestamp = (uint64_t)GetTickCount();
    WaitForSingleObject(mutex, INFINITE);
    next = (write_idx + 1) % MAX_QUEUE;
    if (next != read_idx) {
        queue[write_idx] = s;
        write_idx = next;
        SetEvent(event);
    }
    ReleaseMutex(mutex);
}

void push_openbci_channels(const int32_t chans[RECORDING_MAX_CHANNELS]) {
    RecSample s;
    int next;
    int i;
    if (!recording || g_rec_nch != 8) return;
    s.nch = 8;
    for (i = 0; i < 8; i++)
        s.ch[i] = chans[i];
    s.timestamp = (uint64_t)GetTickCount();
    WaitForSingleObject(mutex, INFINITE);
    next = (write_idx + 1) % MAX_QUEUE;
    if (next != read_idx) {
        queue[write_idx] = s;
        write_idx = next;
        SetEvent(event);
    }
    ReleaseMutex(mutex);
}

BOOL is_recording(void) {
    return recording;
}

const char* recording_last_path(void) {
    return g_last_csv_path;
}
