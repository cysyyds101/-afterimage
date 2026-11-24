#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VOICE_STATE_DISABLED,
    VOICE_STATE_CONNECTING,
    VOICE_STATE_READY,
    VOICE_STATE_LISTENING,
    VOICE_STATE_THINKING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ERROR,
    VOICE_STATE_WIFI_CONNECTED,
    VOICE_STATE_WIFI_DISCONNECTED,
} voice_state_t;

typedef void (*voice_state_callback_t)(voice_state_t state, const char *message);
typedef void (*voice_action_callback_t)(unsigned action);
typedef void (*voice_overlay_callback_t)(const char *url, unsigned duration_ms);

esp_err_t voice_client_start(voice_state_callback_t callback, voice_action_callback_t action_callback);
void voice_client_set_overlay(voice_overlay_callback_t callback);
bool voice_client_enabled(void);
bool voice_client_ready(void);
bool voice_client_wifi_connected(void);
const char *voice_client_token(void);
void voice_client_begin_turn(void);
void voice_client_end_turn(void);
void voice_client_reset_config(void);
