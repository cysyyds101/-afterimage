#pragma once
#include <stdint.h>
#define ANIM_WIDTH 400
#define ANIM_HEIGHT 480
/* Phase 1.3: eight frames resident in PSRAM for the whole run. Four idle
 * micro-motion frames (a blink) plus four static portraits. Boot decodes all
 * eight once; playback only swaps the canvas buffer pointer. */
#define FRAME_COUNT 8
enum {
    FRAME_IDLE_0,
    FRAME_IDLE_1,
    FRAME_IDLE_2,
    FRAME_IDLE_3,
    FRAME_HAPPY,
    FRAME_SLEEP,
    FRAME_LISTENING,
    FRAME_SPEAKING,
};
static const char *const frame_files[FRAME_COUNT] = {
    "idle_00.jpg", "idle_03.jpg", "idle_04.jpg", "idle_05.jpg",
    "wave_00.jpg", "sleep_00.jpg", "idle_01.jpg", "idle_02.jpg",
};
static const char *const frame_names[FRAME_COUNT] = {
    "idle_0", "idle_1", "idle_2", "idle_3", "happy", "sleep", "listening", "speaking",
};
static const uint16_t idle_hold_ms[4] = {2400, 100, 140, 100};