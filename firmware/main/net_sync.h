#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Apply a downloaded binary frame package (PSRAM) into the resident display
 * buffers. Provided by main.c; returns true only if all eight frames decoded
 * and swapped. Flash persistence happens afterwards in net_sync. */
typedef bool (*net_sync_apply_fn)(const uint8_t *bundle, size_t len);
/* Called with true while a character package is downloading/decoding, false
 * when it finishes (success or failure). Lets the UI show 同步中. */
typedef void (*net_sync_status_fn)(bool active);

void net_sync_set_apply(net_sync_apply_fn fn);
void net_sync_set_status(net_sync_status_fn fn);
esp_err_t net_sync_start(void);
/* Clear the stored version so the next poll re-downloads the package (test). */
void net_sync_force(void);