#include <stdio.h>
#include <time.h>
#include "recording.h"

#define MAX_GDF_QUEUE 1024

typedef struct {
    uint16_t ch1;
    uint16_t ch2;
} EEGSample;

static EEGSample queue[MAX_GDF_QUEUE];
static volatile int write_idx = 0;
static volatile int read_idx = 0;
static HANDLE mutex;
static HANDLE event;
static FILE* gdf = NULL;
static volatile BOOL recording = FALSE;

static void write_gdf_header(FILE* f) {
    char hdr[256] = {0};
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    char date[9], time_str[9];
    strftime(date, sizeof(date), "%d.%m.%y", t);
    strftime(time_str, sizeof(time_str), "%H.%M.%S", t);

    snprintf(hdr, 256, "GDF 1.0 EEG Local%-44s%-80s%-8s%-8s%-8s%-8s%-44s%-8s",
        "Patient", "Session", date, date, "1", "2", "O", "n/a");

    fwrite(hdr, 1, 256, f);
}

DWORD WINAPI GdfWriterThread(LPVOID param) {
    while (1) {
        WaitForSingleObject(event, INFINITE);

        while (read_idx != write_idx) {
            EEGSample s;
            WaitForSingleObject(mutex, INFINITE);
            s = queue[read_idx];
            read_idx = (read_idx + 1) % MAX_GDF_QUEUE;
            ReleaseMutex(mutex);

            if (gdf) {
                int16_t buf[2] = { s.ch1, s.ch2 };
                fwrite(buf, sizeof(int16_t), 2, gdf);
            }
        }
    }
    return 0;
}

void init_recording() {
    if (!recording) {
        gdf = fopen("eeg_data.gdf", "wb");
        if (!gdf) return;
        write_gdf_header(gdf);
        mutex = CreateMutex(NULL, FALSE, NULL);
        event = CreateEvent(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, GdfWriterThread, NULL, 0, NULL);
        recording = TRUE;
    }
}

void stop_recording() {
    if (recording) {
        recording = FALSE;
        Sleep(50);
        fclose(gdf);
        gdf = NULL;
    }
}

void push_sample(uint16_t ch1, uint16_t ch2) {
    if (!recording) return;
    WaitForSingleObject(mutex, INFINITE);
    int next = (write_idx + 1) % MAX_GDF_QUEUE;
    if (next != read_idx) {
        queue[write_idx].ch1 = ch1;
        queue[write_idx].ch2 = ch2;
        write_idx = next;
        SetEvent(event);
    }
    ReleaseMutex(mutex);
}

BOOL is_recording() {
    return recording;
}
