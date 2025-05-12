#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>
#include "recording.h"

#define MAX_QUEUE 1024

typedef struct {
    uint16_t ch1;
    uint16_t ch2;
    uint64_t timestamp;
} EEGSample;

static EEGSample queue[MAX_QUEUE];
static volatile int write_idx = 0;
static volatile int read_idx = 0;
static HANDLE mutex;
static HANDLE event;
static FILE* csv = NULL;
static volatile BOOL recording = FALSE;

DWORD WINAPI CsvWriterThread(LPVOID param) {
    while (1) {
        WaitForSingleObject(event, INFINITE);

        while (read_idx != write_idx) {
            EEGSample s;
            WaitForSingleObject(mutex, INFINITE);
            s = queue[read_idx];
            read_idx = (read_idx + 1) % MAX_QUEUE;
            ReleaseMutex(mutex);

            if (csv) {
                fprintf(csv, "%llu,%u,%u\n", s.timestamp, s.ch1, s.ch2);
            }
        }
    }
    return 0;
}

void init_recording() {
    if (!recording) {
        csv = fopen("eeg_data.csv", "w");
        if (!csv) return;

        fprintf(csv, "timestamp,ch1,ch2\n");  // CSV header

        mutex = CreateMutex(NULL, FALSE, NULL);
        event = CreateEvent(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, CsvWriterThread, NULL, 0, NULL);
        recording = TRUE;
    }
}

void stop_recording() {
    if (recording) {
        recording = FALSE;
        Sleep(50);
        fclose(csv);
        csv = NULL;
    }
}

void push_sample(uint16_t ch1, uint16_t ch2) {
    if (!recording) return;
    WaitForSingleObject(mutex, INFINITE);
    int next = (write_idx + 1) % MAX_QUEUE;
    if (next != read_idx) {
        queue[write_idx].ch1 = ch1;
        queue[write_idx].ch2 = ch2;
        queue[write_idx].timestamp = (uint64_t)GetTickCount();
        write_idx = next;
        SetEvent(event);
    }
    ReleaseMutex(mutex);
}

BOOL is_recording() {
    return recording;
}
