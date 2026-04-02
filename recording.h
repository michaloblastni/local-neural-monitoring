#pragma once
#include <windows.h>
#include <stdint.h>

#define RECORDING_MAX_CHANNELS 8

/*
 * init_recording(num_channels)
 * ---------------------------
 * num_channels: 2 for EEG-SMT, 8 for OpenBCI Cyton.
 * Creates eeg_data.csv with header timestamp,ch1,...chN
 */

void init_recording(int num_channels);

void stop_recording(void);

void push_sample(uint16_t ch1, uint16_t ch2);

void push_openbci_channels(const int32_t chans[RECORDING_MAX_CHANNELS]);

BOOL is_recording(void);

/* Returns the absolute path of the last CSV written (empty string if none). */
const char* recording_last_path(void);
