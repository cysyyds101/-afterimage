/* Phase 1 firmware: one BOOT key, no touch, no on-screen Wi-Fi UI, no offline
 * speech inference. Eight frames are decoded into PSRAM once at boot and the
 * canvas only ever swaps between those resident buffers.
 * GPIO0: hold >150 ms records and uploads; release sends; hold >=6 s wipes the
 * saved configuration and reboots into the phone provisioning AP.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "animation_manifest.h"
#include "voice_client.h"
#include "net_sync.h"

#define FRAME_BYTES (ANIM_WIDTH * ANIM_HEIGHT * sizeof(uint16_t))
#define JPEG_CAPACITY (128 * 1024)
#define BOOT_PIN GPIO_NUM_0
#define DEBOUNCE_MS 35
#define TALK_HOLD_MS 150
#define RESET_HOLD_MS 6000
#define PORTRAIT_WAKE_MS 4000
/* Phase 1 acceptance budget: a bind must never exceed 15 ms. */
#define FRAME_BUDGET_US 15000
#define FRAME_STATS_PERIOD 100

LV_FONT_DECLARE(lv_font_cn_18);

typedef enum {
    CMD_IDLE,
    CMD_NEXT,
    CMD_VOICE_READY,
    CMD_VOICE_LISTENING,
    CMD_VOICE_THINKING,
    CMD_VOICE_SPEAKING,
    CMD_WAKE,
    CMD_ACT_WAVE,
    CMD_ACT_JUMP,
    CMD_ACT_SLEEP,
    CMD_NOP,
} command_t;
static const char *TAG = "whale_maid";
static QueueHandle_t commands;
static lv_obj_t *canvas, *status_label;
static uint16_t *resident[FRAME_COUNT];
static uint8_t *jpeg_bytes;
static uint8_t *frame_staging;
static jpeg_dec_handle_t decoder;

/* Temporary display overlay (e.g. weather card) pushed mid-conversation.
 * The decoded frame is allocated per message and released on dismissal or when
 * replaced, so repeated overlays cannot leak. `overlay_front` is the image
 * currently bound to the canvas; `overlay_next` is decoded and waiting to be
 * swapped in by playback_task. */
typedef struct { char url[256]; uint32_t duration_ms; } overlay_req_t;
static uint16_t *overlay_front;
static uint16_t *overlay_next;
static QueueHandle_t overlay_queue;
static esp_lcd_touch_handle_t touch_handle;
static portMUX_TYPE overlay_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool overlay_pending;
static volatile bool overlay_active;
static volatile bool overlay_dismiss_request;
static volatile bool overlay_release_front;
static volatile uint32_t overlay_generation;
static volatile uint32_t overlay_pending_generation;
static volatile uint32_t overlay_duration_ms;
static int64_t overlay_until;
static volatile voice_state_t voice_visual_state = VOICE_STATE_DISABLED;
static char voice_status[96] = "";
static volatile bool status_dirty;
static volatile bool net_sync_busy;

static void send_command(command_t cmd)
{
    if (xQueueSend(commands, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "Input queue full; ignored extra click");
}

static void show_status(const char *message)
{
    if (bsp_display_lock(1000)) {
        lv_label_set_text(status_label, message);
        lv_obj_move_foreground(status_label);
        bsp_display_unlock();
    }
}

/* Short Chinese state word for the corner capsule. Detailed messages go to the
 * serial log only; the provisioning hint is the one multi-line exception. */
static const char *state_word(void)
{
    switch (voice_visual_state) {
        case VOICE_STATE_LISTENING: return "聆听中";
        case VOICE_STATE_THINKING: return "思考中";
        case VOICE_STATE_SPEAKING: return "说话中";
        case VOICE_STATE_CONNECTING:
            return strstr(voice_status, "手机配网") ? voice_status : "连接中";
        case VOICE_STATE_WIFI_DISCONNECTED: return "未联网";
        case VOICE_STATE_ERROR: return "服务异常";
        default: return net_sync_busy ? "同步中" : "待机";
    }
}

static void net_sync_status(bool active)
{
    net_sync_busy = active;
    status_dirty = true;
    send_command(CMD_NOP);
}

/* Leave a persistent overlay at the instant a new turn starts. Bumps the
 * generation so a card still downloading/decoding is dropped, not shown. */
static void overlay_cancel_for_turn(void)
{
    taskENTER_CRITICAL(&overlay_mux);
    overlay_dismiss_request = true;
    overlay_generation++;
    taskEXIT_CRITICAL(&overlay_mux);
}

static void voice_state_changed(voice_state_t state, const char *message)
{
    voice_visual_state = state;
    if (message && message[0]) strlcpy(voice_status, message, sizeof(voice_status));
    else voice_status[0] = '\0';
    ESP_LOGI(TAG, "Voice state %d: %s", (int)state, voice_status[0] ? voice_status : "-");
    command_t cmd;
    switch (state) {
        case VOICE_STATE_LISTENING:
            /* A new turn starts here (BOOT held): leave any persistent overlay
             * immediately, cancel in-flight loads, and resume the animation. */
            overlay_cancel_for_turn();
            cmd = CMD_VOICE_LISTENING;
            break;
        case VOICE_STATE_THINKING: cmd = CMD_VOICE_THINKING; break;
        case VOICE_STATE_SPEAKING: cmd = CMD_VOICE_SPEAKING; break;
        /* Ready, connecting, error and Wi-Fi loss all return to the idle
         * animation. The label itself is refreshed by playback_task, never
         * here: this callback runs inside the WebSocket event task while it
         * holds the client lock and must not take the display lock. */
        default: cmd = CMD_VOICE_READY; break;
    }
    status_dirty = true;
    send_command(cmd);
}

static void voice_action_changed(unsigned action)
{
    switch (action) {
        case 1: send_command(CMD_ACT_WAVE); break;
        case 2: send_command(CMD_ACT_JUMP); break;
        case 3: send_command(CMD_ACT_SLEEP); break;
        default: break;
    }
}

/* BOOT single-key state machine. A press only becomes a recording after
 * TALK_HOLD_MS so a tap cannot open a turn; a 6 s hold is the deliberate
 * configuration wipe and wins over the recording on release. */
static void button_task(void *arg)
{
    (void)arg;
    int raw_last = gpio_get_level(BOOT_PIN), stable = raw_last;
    int64_t changed = esp_timer_get_time(), pressed = changed;
    bool armed = stable == 1, talking = false, resetting = false;
    for (;;) {
        int raw = gpio_get_level(BOOT_PIN);
        int64_t now = esp_timer_get_time();
        if (raw != raw_last) { raw_last = raw; changed = now; }
        if (raw != stable && now - changed >= DEBOUNCE_MS * 1000LL) {
            stable = raw;
            if (!stable) {
                pressed = now;
                talking = false;
                resetting = false;
                ESP_LOGI(TAG, "BOOT pressed (wifi=%d socket=%d)",
                         voice_client_wifi_connected(), voice_client_ready());
            } else {
                ESP_LOGI(TAG, "BOOT released (held %lld ms, talking=%d, wifi=%d, socket=%d)",
                         (long long)((now - pressed) / 1000), talking,
                         voice_client_wifi_connected(), voice_client_ready());
                if (talking) voice_client_end_turn();
                talking = false;
                armed = true; /* Ignore a key already held at startup. */
            }
        }
        if (armed && !stable) {
            int64_t held = now - pressed;
            if (!talking && !resetting && held >= TALK_HOLD_MS * 1000LL) {
                talking = true;
                voice_client_begin_turn();
            }
            if (!resetting && held >= RESET_HOLD_MS * 1000LL) {
                resetting = true;
                talking = false;
                voice_client_reset_config();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Boot-only decode. Runtime playback never touches SPIFFS: the 60-122 ms
 * open() scans measured in Phase 0 were the entire source of switch stalls. */
static bool decode_jpeg(const char *path, uint16_t *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ESP_LOGE(TAG, "Missing %s", path); return false; }
    size_t size = 0;
    while (size < JPEG_CAPACITY) {
        ssize_t got = read(fd, jpeg_bytes + size, JPEG_CAPACITY - size);
        if (got < 0) { size = 0; break; }
        if (got == 0) break;
        size += (size_t)got;
    }
    close(fd);
    if (size == 0 || size == JPEG_CAPACITY) {
        ESP_LOGE(TAG, "Invalid/oversize JPEG: %s", path);
        return false;
    }
    jpeg_dec_io_t io = { .inbuf = jpeg_bytes, .inbuf_len = (int)size };
    jpeg_dec_header_info_t header = {0};
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
        header.width != ANIM_WIDTH || header.height != ANIM_HEIGHT) {
        ESP_LOGE(TAG, "Bad dimensions/JPEG header: %s", path);
        return false;
    }
    int output_length = 0;
    jpeg_dec_get_outbuf_len(decoder, &output_length);
    if (output_length <= 0 || output_length > FRAME_BYTES) return false;
    io.outbuf = (uint8_t *)out;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Decode failed: %s", path);
        return false;
    }
    return true;
}

static bool display_frame(unsigned slot)
{
    int64_t t_bind = esp_timer_get_time();
    if (!bsp_display_lock(1000)) return false;
    int64_t t_lock = esp_timer_get_time();
    lv_canvas_set_buffer(canvas, resident[slot], ANIM_WIDTH, ANIM_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
    lv_label_set_text(status_label, state_word());
    bsp_display_unlock();
    int64_t t_done = esp_timer_get_time();
    if (t_done - t_bind > FRAME_BUDGET_US)
        ESP_LOGW(TAG, "Display cost %s: lvgl_lock=%lld bind=%lld total=%lld us",
                 frame_names[slot], (long long)(t_lock - t_bind),
                 (long long)(t_done - t_lock), (long long)(t_done - t_bind));
    return true;
}

/* Decode a single downloaded binary package (magic "WMB1", u32 count, then
 * count * [u32 len + JPEG]) straight from PSRAM into the resident slots, so the
 * character switches immediately without any flash read. Flash persistence is
 * handled separately by net_sync after the display swap. */
static bool apply_bundle(const uint8_t *bundle, size_t len)
{
    if (!frame_staging || len < 8 || memcmp(bundle, "WMB1", 4) != 0) return false;
    uint32_t count;
    memcpy(&count, bundle + 4, 4);
    if (count != FRAME_COUNT) return false;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&cfg, &decoder) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Sync JPEG decoder open failed");
        return false;
    }
    size_t off = 8;
    bool ok = true;
    for (unsigned slot = 0; slot < FRAME_COUNT; slot++) {
        if (off + 4 > len) { ok = false; break; }
        uint32_t fl;
        memcpy(&fl, bundle + off, 4);
        off += 4;
        if (fl == 0 || fl > JPEG_CAPACITY || off + fl > len) { ok = false; break; }
        jpeg_dec_io_t io = { .inbuf = (uint8_t *)(bundle + off), .inbuf_len = (int)fl };
        jpeg_dec_header_info_t header = {0};
        if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
            header.width != ANIM_WIDTH || header.height != ANIM_HEIGHT) { ok = false; break; }
        int output_length = 0;
        jpeg_dec_get_outbuf_len(decoder, &output_length);
        if (output_length <= 0 || output_length > FRAME_BYTES) { ok = false; break; }
        io.outbuf = (uint8_t *)frame_staging;
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) { ok = false; break; }
        if (bsp_display_lock(1000)) {
            memcpy(resident[slot], frame_staging, FRAME_BYTES);
            bsp_display_unlock();
        }
        off += fl;
    }
    jpeg_dec_close(decoder);
    decoder = NULL;
    if (ok && bsp_display_lock(1000)) {
        lv_obj_invalidate(canvas);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "Apply bundle: %s", ok ? "ok" : "failed");
    return ok;
}

static void voice_overlay_changed(const char *url, unsigned duration_ms)
{
    overlay_req_t req = {0};
    strlcpy(req.url, url, sizeof(req.url));
    req.duration_ms = duration_ms;  // 0 = stay until the next turn starts
    if (overlay_queue && xQueueSend(overlay_queue, &req, 0) != pdTRUE)
        ESP_LOGW(TAG, "Overlay queue full; drop");
}

typedef struct { uint8_t *buf; size_t len, cap; bool overflow; } overlay_http_ctx_t;

static esp_err_t overlay_http_event(esp_http_client_event_t *evt)
{
    overlay_http_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data_len > 0) {
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
        } else ctx->overflow = true;
    }
    return ESP_OK;
}

/* Returns downloaded byte count, or -1 on failure. LAN HTTP, no auth needed. */
static int overlay_download(const char *url, uint8_t *jpeg)
{
    overlay_http_ctx_t ctx = { .buf = jpeg, .cap = JPEG_CAPACITY };
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 15000,
        .event_handler = overlay_http_event, .user_data = &ctx, .buffer_size = 16384,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t t1 = esp_timer_get_time();
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || ctx.overflow || ctx.len == 0) {
        ESP_LOGW(TAG, "Overlay download failed: err=%s status=%d bytes=%u",
                 esp_err_to_name(err), status, (unsigned)ctx.len);
        return -1;
    }
    ESP_LOGI(TAG, "Overlay downloaded %u bytes in %lld ms", (unsigned)ctx.len, (long long)(t1 - t0) / 1000);
    return (int)ctx.len;
}

static void overlay_task(void *arg)
{
    (void)arg;
    overlay_req_t req;
    for (;;) {
        if (xQueueReceive(overlay_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        uint32_t generation;
        taskENTER_CRITICAL(&overlay_mux);
        generation = overlay_generation;
        taskEXIT_CRITICAL(&overlay_mux);

        uint8_t *jpeg = heap_caps_malloc(JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg) { ESP_LOGW(TAG, "Overlay JPEG alloc failed"); continue; }
        int len = overlay_download(req.url, jpeg);
        if (len <= 0) { free(jpeg); continue; }

        uint16_t *frame = heap_caps_aligned_alloc(16, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame) { ESP_LOGW(TAG, "Overlay frame alloc failed"); free(jpeg); continue; }
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        jpeg_dec_handle_t dec = NULL;
        if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "Overlay decoder open failed");
            free(frame);
            free(jpeg);
            continue;
        }
        jpeg_dec_io_t io = { .inbuf = jpeg, .inbuf_len = len };
        jpeg_dec_header_info_t header = {0};
        bool ok = jpeg_dec_parse_header(dec, &io, &header) == JPEG_ERR_OK &&
                  header.width == ANIM_WIDTH && header.height == ANIM_HEIGHT;
        if (ok) {
            int outlen = 0;
            jpeg_dec_get_outbuf_len(dec, &outlen);
            ok = outlen > 0 && outlen <= FRAME_BYTES;
        }
        if (ok) {
            io.outbuf = (uint8_t *)frame;
            ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
        }
        jpeg_dec_close(dec);
        free(jpeg);  /* download scratch released as soon as decoding is done */

        bool stale;
        taskENTER_CRITICAL(&overlay_mux);
        stale = generation != overlay_generation;
        taskEXIT_CRITICAL(&overlay_mux);
        if (!ok || stale) {
            if (!ok) ESP_LOGW(TAG, "Overlay decode failed");
            else ESP_LOGI(TAG, "Overlay cancelled by a new turn");
            free(frame);
            continue;
        }

        uint16_t *replaced = NULL;
        taskENTER_CRITICAL(&overlay_mux);
        replaced = overlay_next;
        overlay_next = frame;
        overlay_duration_ms = req.duration_ms;
        overlay_pending_generation = generation;
        overlay_pending = true;
        taskEXIT_CRITICAL(&overlay_mux);
        if (replaced) free(replaced);  /* release an unconsumed previous image before publishing */
        send_command(CMD_NOP);
        ESP_LOGI(TAG, "Overlay ready (%s)", req.duration_ms ? "timed" : "persistent");
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    bool idle_mode = true;
    unsigned idle_frame = 0, portrait = FRAME_HAPPY;
    int64_t portrait_until = 0;
    bool need_bind = true;
    unsigned next_cycle = 0;
    uint32_t stat_count = 0, stat_overrun = 0;
    int64_t stat_min = INT64_MAX, stat_max = 0, stat_sum = 0;
    for (;;) {
        /* Refresh the corner capsule here (the only task that owns UI writes
         * at runtime), so state callbacks never block on the display lock. */
        if (status_dirty) {
            status_dirty = false;
            if (bsp_display_lock(1000)) {
                lv_label_set_text(status_label, state_word());
                bsp_display_unlock();
            }
        }
        /* Overlay插播: swap in the newest decoded card. duration_ms=0 keeps it
         * on screen until the next turn starts (or a tap dismisses it). */
        if (overlay_pending) {
            uint16_t *next = NULL;
            uint32_t pending_gen = 0, current_gen = 0;
            taskENTER_CRITICAL(&overlay_mux);
            if (overlay_pending) {
                next = overlay_next;
                pending_gen = overlay_pending_generation;
                overlay_next = NULL;
                overlay_pending = false;
            }
            current_gen = overlay_generation;
            taskEXIT_CRITICAL(&overlay_mux);
            if (next && pending_gen != current_gen) {
                free(next);  /* a new turn started while this card was readying */
                next = NULL;
                ESP_LOGI(TAG, "Overlay dropped (new turn started)");
            }
            if (next) {
                uint16_t *old = overlay_front;
                overlay_front = next;
                overlay_active = true;
                overlay_dismiss_request = false;  /* stale request from this turn's BOOT press */
                overlay_release_front = false;  /* this swap releases the old front */
                overlay_until = overlay_duration_ms
                    ? esp_timer_get_time() + (int64_t)overlay_duration_ms * 1000
                    : INT64_MAX;
                if (bsp_display_lock(1000)) {
                    lv_canvas_set_buffer(canvas, overlay_front, ANIM_WIDTH, ANIM_HEIGHT, LV_COLOR_FORMAT_RGB565);
                    lv_obj_invalidate(canvas);
                    bsp_display_unlock();
                }
                if (old) free(old);  /* release the previously displayed card */
                if (overlay_duration_ms)
                    ESP_LOGI(TAG, "Overlay shown for %u ms", (unsigned)overlay_duration_ms);
                else
                    ESP_LOGI(TAG, "Overlay shown (persistent)");
            }
        }
        if (overlay_active) {
            bool tap = false;
            if (touch_handle) {
                esp_lcd_touch_point_data_t point;
                uint8_t cnt = 0;
                /* FT5x06 can NAK the first poll after an idle/sleep window; retry once. */
                if (esp_lcd_touch_read_data(touch_handle) != ESP_OK)
                    esp_lcd_touch_read_data(touch_handle);
                esp_lcd_touch_get_data(touch_handle, &point, &cnt, 1);
                tap = cnt > 0;
            }
            bool timed_out = overlay_duration_ms && esp_timer_get_time() >= overlay_until;
            bool boot = overlay_dismiss_request;
            if (tap || boot || timed_out) {
                overlay_active = false;
                overlay_dismiss_request = false;
                uint16_t *drop = NULL;
                taskENTER_CRITICAL(&overlay_mux);
                drop = overlay_next;
                overlay_next = NULL;
                overlay_pending = false;
                taskEXIT_CRITICAL(&overlay_mux);
                if (drop) free(drop);  /* cancel a card that arrived mid-dismiss */
                overlay_release_front = true;  /* freed after the canvas rebinds */
                need_bind = true;
                ESP_LOGI(TAG, "Overlay dismissed (%s); resume animation",
                         tap ? "touch" : boot ? "boot" : "timeout");
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }
        int64_t started = esp_timer_get_time();
        unsigned slot = idle_mode ? idle_frame : portrait;
        if (need_bind) {
            bool ok = display_frame(slot);
            need_bind = false;
            int64_t elapsed = esp_timer_get_time() - started;
            if (ok) {
                stat_count++;
                stat_sum += elapsed;
                if (elapsed < stat_min) stat_min = elapsed;
                if (elapsed > stat_max) stat_max = elapsed;
                if (elapsed > FRAME_BUDGET_US) {
                    stat_overrun++;
                    ESP_LOGW(TAG, "Frame slow: %lld us", (long long)elapsed);
                }
                if (stat_count >= FRAME_STATS_PERIOD) {
                    ESP_LOGI(TAG, "Frame stats: n=%u min=%lld avg=%lld max=%lld us overrun=%u resident=on",
                             (unsigned)stat_count, (long long)stat_min,
                             (long long)(stat_sum / stat_count), (long long)stat_max,
                             (unsigned)stat_overrun);
                    stat_count = 0; stat_sum = 0; stat_overrun = 0;
                    stat_min = INT64_MAX; stat_max = 0;
                    ESP_LOGI(TAG, "Heap trend: internal=%u dma_block=%u psram=%u",
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
            } else {
                show_status("显示绑定失败");
            }
            if (ok && overlay_release_front) {
                overlay_release_front = false;
                if (overlay_front) {
                    free(overlay_front);
                    overlay_front = NULL;
                    ESP_LOGI(TAG, "Overlay image released");
                }
            }
        }
        int64_t now = esp_timer_get_time();
        int64_t wait_us;
        if (idle_mode) {
            wait_us = idle_hold_ms[idle_frame] * 1000LL;
        } else if (portrait_until && now >= portrait_until) {
            idle_mode = true;
            idle_frame = 0;
            portrait_until = 0;
            need_bind = true;
            ESP_LOGI(TAG, "Portrait hold expired; idle");
            continue;
        } else {
            wait_us = portrait_until ? portrait_until - now : 1000000LL;
            if (wait_us > 1000000LL) wait_us = 1000000LL;
        }
        command_t cmd;
        TickType_t wait = pdMS_TO_TICKS((wait_us + 999) / 1000);
        if (xQueueReceive(commands, &cmd, wait ? wait : 1) == pdTRUE) {
            do {
                switch (cmd) {
                    case CMD_IDLE:
                    case CMD_VOICE_READY:
                        idle_mode = true; idle_frame = 0; portrait_until = 0;
                        break;
                    case CMD_VOICE_LISTENING:
                    case CMD_VOICE_THINKING:
                        idle_mode = false; portrait = FRAME_LISTENING; portrait_until = 0;
                        break;
                    case CMD_VOICE_SPEAKING:
                        idle_mode = false; portrait = FRAME_SPEAKING; portrait_until = 0;
                        break;
                    case CMD_WAKE:
                    case CMD_ACT_WAVE:
                        idle_mode = false; portrait = FRAME_HAPPY;
                        portrait_until = esp_timer_get_time() + PORTRAIT_WAKE_MS * 1000LL;
                        break;
                    case CMD_ACT_JUMP:
                        idle_mode = false; portrait = FRAME_SPEAKING;
                        portrait_until = esp_timer_get_time() + PORTRAIT_WAKE_MS * 1000LL;
                        break;
                    case CMD_ACT_SLEEP:
                        idle_mode = false; portrait = FRAME_SLEEP; portrait_until = 0;
                        break;
                    case CMD_NOP:
                        break;
                    case CMD_NEXT: {
                        static const unsigned cycle[] = {FRAME_HAPPY, FRAME_SPEAKING, FRAME_SLEEP, FRAME_LISTENING};
                        if (next_cycle == 0) { idle_mode = true; idle_frame = 0; }
                        else { idle_mode = false; portrait = cycle[next_cycle - 1]; }
                        portrait_until = 0;
                        next_cycle = (next_cycle + 1) % 5;
                        break;
                    }
                }
            } while (xQueueReceive(commands, &cmd, 0) == pdTRUE);
            need_bind = true;
            ESP_LOGI(TAG, "Selected %s", idle_mode ? "idle" : frame_names[portrait]);
        } else if (idle_mode) {
            idle_frame = (idle_frame + 1) % 4;
            need_bind = true;
        }
    }
}

/* Reproducible board-side test channel: drives the same command queue as the
 * BOOT key and the cloud state callbacks, so a scripted soak is repeatable. */
static void console_task(void *arg)
{
    (void)arg;
    /* CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y, so esp_stdio already installed this
     * driver at system init; installing again returns ESP_ERR_INVALID_STATE. */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Test console unavailable: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "Test console ready (next/idle/wave/jump/sleep/wake/happy/listen/speak/overlay/talk/endtalk)");
    char line[280];
    size_t used = 0;
    for (;;) {
        uint8_t byte;
        int got = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(200));
        if (got <= 0) continue;
        if (byte == '\r') continue;
        if (byte != '\n') {
            if (used < sizeof(line) - 1) line[used++] = (char)byte;
            continue;
        }
        line[used] = '\0';
        used = 0;
        if (!strncmp(line, "overlay", 7)) {
            char url[256] = {0};
            unsigned ms = 12000;
            int n = sscanf(line, "overlay %255s %u", url, &ms);
            if (n >= 1 && url[0]) {
                ESP_LOGI(TAG, "Test overlay: %s (%u ms)", url, ms);
                voice_overlay_changed(url, ms);
            } else {
                ESP_LOGW(TAG, "Usage: overlay <url> [ms]");
            }
            continue;
        }
        if (!strcmp(line, "resync")) {
            ESP_LOGI(TAG, "Test command: resync");
            net_sync_force();
            continue;
        }
        /* Same entry points as the physical BOOT hold/release, so the turn
         * start gating and end-of-turn reset can be tested without a press. */
        if (!strcmp(line, "talk")) {
            ESP_LOGI(TAG, "Test command: talk (BOOT hold path)");
            voice_client_begin_turn();
            continue;
        }
        if (!strcmp(line, "endtalk")) {
            ESP_LOGI(TAG, "Test command: endtalk (BOOT release path)");
            voice_client_end_turn();
            continue;
        }
        command_t cmd;
        if (!strcmp(line, "next")) cmd = CMD_NEXT;
        else if (!strcmp(line, "idle") || !strcmp(line, "home")) cmd = CMD_IDLE;
        else if (!strcmp(line, "wave") || !strcmp(line, "happy")) cmd = CMD_ACT_WAVE;
        else if (!strcmp(line, "jump") || !strcmp(line, "speak")) cmd = CMD_ACT_JUMP;
        else if (!strcmp(line, "sleep")) cmd = CMD_ACT_SLEEP;
        else if (!strcmp(line, "listen")) { cmd = CMD_VOICE_LISTENING; overlay_cancel_for_turn(); }
        else if (!strcmp(line, "wake")) cmd = CMD_WAKE;
        else if (!line[0]) continue;
        else { ESP_LOGW(TAG, "Unknown test command: %s", line); continue; }
        ESP_LOGI(TAG, "Test command: %s", line);
        send_command(cmd);
    }
}

/* The BSP's bsp_display_start_with_config() unconditionally creates the FT5x06
 * touch input, which shares the I2C bus with both audio codecs and the reset
 * line with the panel. Phase 1 removes touch entirely, so the display is built
 * from the public BSP/lvgl-port pieces instead, keeping the three anti-glitch
 * rules: internal DMA draw buffer, 40 MHz QSPI (SH8601 macro) and 2-pixel
 * rounded refresh areas. */
static void rounder_event_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static lv_display_t *display_start_no_touch(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 0; /* display on core 0, network/audio on core 1 */
    if (lvgl_port_init(&port_cfg) != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&disp_config, &panel, &io) != ESP_OK) return NULL;
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT,
        .monochrome = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .sw_rotate = true, .buff_dma = false, .buff_spiram = false, .swap_bytes = true },
    };
    /* QSPI is an SPI panel. add_disp_rgb would register RGB vsync callbacks
     * into the SPI panel object (IDF does not type-check that call) and would
     * call flush_ready before the SPI DMA completes; add_disp wires the SPI
     * on_color_trans_done callback instead. */
    lv_display_t *display = lvgl_port_add_disp(&disp_cfg);
    if (!display) return NULL;
    lv_display_add_event_cb(display, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    if (bsp_display_brightness_init() != ESP_OK) return NULL;
    return display;
}

void app_main(void)
{
    commands = xQueueCreate(8, sizeof(command_t));
    if (!commands) { ESP_LOGE(TAG, "Queue allocation failed"); return; }
    lv_display_t *display = display_start_no_touch();
    if (!display) { ESP_LOGE(TAG, "Display start failed"); return; }
    /* Pin LVGL's draw buffer to internal DMA RAM once at boot so no QSPI flush
     * ever needs a temporary internal allocation (Phase 0 boot flicker). */
    const size_t draw_bytes = (size_t)BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT * 2;
    void *draw_dma = heap_caps_aligned_alloc(16, draw_bytes,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (draw_dma) {
        bsp_display_lock(0);
        lv_display_set_buffers(display, draw_dma, NULL, draw_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
        bsp_display_unlock();
        ESP_LOGI(TAG, "LVGL draw buffer pinned to internal DMA (%u bytes)", (unsigned)draw_bytes);
    } else {
        ESP_LOGW(TAG, "Internal DMA draw buffer unavailable; flushes may allocate per transfer");
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(65));
    for (unsigned slot = 0; slot < FRAME_COUNT; slot++) {
        resident[slot] = heap_caps_aligned_calloc(16, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!resident[slot]) { ESP_LOGE(TAG, "PSRAM frame allocation failed"); return; }
    }
    jpeg_bytes = heap_caps_malloc(JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_bytes) { ESP_LOGE(TAG, "PSRAM buffer allocation failed"); return; }
    frame_staging = heap_caps_aligned_alloc(16, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_staging) ESP_LOGW(TAG, "Staging buffer unavailable; network sync disabled");
    /* Overlay images are allocated per message and freed on dismissal, so no
     * permanent PSRAM is reserved while no card is on screen. */
    overlay_queue = xQueueCreate(2, sizeof(overlay_req_t));
    if (!overlay_queue)
        ESP_LOGW(TAG, "Overlay queue unavailable; overlay disabled");
    if (bsp_touch_new(NULL, &touch_handle) != ESP_OK) {
        touch_handle = NULL;
        ESP_LOGW(TAG, "Touch unavailable; dismiss overlay by BOOT only");
    }
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, resident[0], ANIM_WIDTH, ANIM_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);
    /* Corner capsule: 24 px inset clears the AMOLED rounded corners, the dark
     * semi-transparent pill keeps white text readable over any frame or card,
     * and the top-left position stays clear of the character's face. */
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_cn_18, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(status_label, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_70, 0);
    lv_obj_set_style_radius(status_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(status_label, 14, 0);
    lv_obj_set_style_pad_ver(status_label, 7, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 24, 24);
    lv_label_set_text(status_label, "加载中…");
    bsp_display_unlock();
    esp_vfs_spiffs_conf_t fs = {
        .base_path = "/storage", .partition_label = "storage", .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS: %s", esp_err_to_name(err));
        show_status("缺少素材\n请烧录存储镜像");
        return;
    }
    jpeg_dec_config_t jpeg_cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&jpeg_cfg, &decoder) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG decoder failed"); show_status("解码器初始化失败"); return;
    }
    net_sync_set_apply(apply_bundle);
    net_sync_set_status(net_sync_status);
    if (net_sync_start() != ESP_OK) ESP_LOGW(TAG, "Network sync unavailable; local playback only");
    const char *src_dir = "/storage";
    if (access("/model/version.txt", F_OK) == 0) {
        bool all = true;
        for (unsigned slot = 0; slot < FRAME_COUNT; slot++) {
            char p[96];
            snprintf(p, sizeof(p), "/model/%s", frame_files[slot]);
            if (access(p, F_OK) != 0) { all = false; break; }
        }
        if (all) src_dir = "/model";
    }
    ESP_LOGI(TAG, "Boot frames from %s", src_dir);
    int64_t t_decode = esp_timer_get_time();
    for (unsigned slot = 0; slot < FRAME_COUNT; slot++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", src_dir, frame_files[slot]);
        if (!decode_jpeg(path, resident[slot])) {
            show_status("素材解码失败\n详见串口日志");
            return;
        }
    }
    jpeg_dec_close(decoder);
    decoder = NULL;
    ESP_LOGI(TAG, "Resident frames: %u decoded in %lld ms; free internal=%u dma_block=%u PSRAM=%u",
             (unsigned)FRAME_COUNT, (long long)((esp_timer_get_time() - t_decode) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOOT_PIN, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    /* Priority 6 (same as the audio tasks): even a busy display pipeline must
     * never delay press/release detection; the 10 ms poll yields between runs. */
    if (xTaskCreate(button_task, "button", 3072, NULL, 6, NULL) != pdPASS ||
        xTaskCreatePinnedToCore(playback_task, "animation", 8192, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed"); show_status("任务初始化失败");
    }
    if (overlay_queue && xTaskCreate(overlay_task, "overlay", 8192, NULL, 4, NULL) != pdPASS)
        ESP_LOGW(TAG, "Overlay task creation failed");
    if (xTaskCreate(console_task, "test_console", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "Test console task creation failed");
    voice_client_set_overlay(voice_overlay_changed);
    if (voice_client_enabled()) {
        esp_err_t voice_err = voice_client_start(voice_state_changed, voice_action_changed);
        if (voice_err != ESP_OK) {
            ESP_LOGE(TAG, "Voice init failed: %s", esp_err_to_name(voice_err));
            show_status("语音初始化失败");
        }
    }
}