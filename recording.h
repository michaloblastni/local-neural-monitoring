#pragma once
#include <windows.h>
#include <stdint.h>

void init_recording();
void stop_recording();
void push_sample(uint16_t ch1, uint16_t ch2);
BOOL is_recording();

