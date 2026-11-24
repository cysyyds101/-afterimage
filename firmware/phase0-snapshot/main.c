/* Eight keyframes per action. JPEG assets and timing come from export_assets.py.
 * GPIO0: short release -> next action; hold >=800 ms -> idle.
 * A screen tap also selects the next action. PWR retains its hardware function.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_jpeg_dec.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "animation_manifest.h"
#include "voice_client.h"

#define FRAME_BYTES (ANIM_WIDTH * ANIM_HEIGHT * sizeof(uint16_t))
#define JPEG_CAPACITY (128 * 1024)
#define BOOT_PIN GPIO_NUM_0
#define DEBOUNCE_MS 35
#define HOLD_MS 800
/* Report render-cost statistics every this many frames (~20 s at idle pacing). */
#define FRAME_STATS_PERIOD 100

typedef enum {
    CMD_NEXT,
    CMD_HOME,
    CMD_VOICE_READY,
    CMD_VOICE_LISTENING,
    CMD_VOICE_THINKING,
    CMD_VOICE_SPEAKING,
    CMD_WAKE,
    CMD_ACT_WAVE,
    CMD_ACT_JUMP,
    CMD_ACT_SLEEP,
} command_t;
static const char *TAG = "whale_maid";
static QueueHandle_t commands;
static lv_obj_t *canvas, *status_label;
static lv_obj_t *wifi_panel, *wifi_list, *wifi_info_label;
static lv_obj_t *wifi_password_dialog, *wifi_password_input, *wifi_password_title;
static char selected_wifi_ssid[33];
static uint16_t *scratch_pixels[2];
static unsigned scratch_index;
static uint16_t *frame_cache[ANIM_FRAMES];
static bool cache_enabled;
static atomic_int cache_action = -1;
static atomic_uint cache_mask;
/* Interlock between playback (core 0, prio 5) and the cache task (core 0, prio 3).
 * cache_busy is held only across the buffer bind, never across a decode, so the
 * cache task never waits on flash I/O. bound_frame/stage_bound tell the cache
 * task which PSRAM buffer the canvas is currently showing. */
static atomic_bool cache_busy;
static atomic_int bound_frame = -1;
static atomic_bool stage_bound;
static atomic_bool stage_ready;
static atomic_int stage_action = -1;
static atomic_int decode_request;
/* Accumulated decode cost for the current cache fill. Logging one line per
 * frame flooded the UART during an action switch (16 lines ~ 150 ms at
 * 115200 baud), which both distorted the measurement and competed with the
 * playback task. One summary per action keeps the evidence and the noise down. */
static atomic_int decode_cost_us;
static atomic_int decode_cost_frames;
static uint8_t *jpeg_bytes;
static jpeg_dec_handle_t decoder;
static SemaphoreHandle_t decoder_lock;
#define CACHE_FULL_MASK ((1u << ANIM_FRAMES) - 1)
static volatile voice_state_t voice_visual_state = VOICE_STATE_DISABLED;
static char voice_status[96] = "";
static void wifi_network_button_cb(lv_event_t *event);

static void send_command(command_t cmd)
{
    if (xQueueSend(commands, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "Input queue full; ignored extra click");
}

/* Phase 0.1 reset attribution. Two spontaneous rst:0xc reboots were captured with
 * nobody near the board (p0-scripted-630s.log, 257922 ms and 10431 ms). Each was
 * preceded by idle->wave->jump->sleep, which only CMD_NEXT produces, i.e. tap_cb.
 * report() does not log, so the reset path was invisible. Log every touch event
 * with its coordinates so phantom FT3168 presses can be told from real ones. */
static void log_touch(const char *what, lv_event_t *event)
{
    lv_point_t point = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &point);
    ESP_LOGW(TAG, "Touch %s at (%d,%d) event=%d", what,
             (int)point.x, (int)point.y, event ? (int)lv_event_get_code(event) : -1);
}

static void tap_cb(lv_event_t *event)
{
    log_touch("CLICKED", event);
    send_command(CMD_NEXT);
}

static void setup_hold_cb(lv_event_t *event)
{
    log_touch("LONG_PRESSED", event);
    if (voice_client_enabled()) voice_client_reset_config();
}

static void show_status(const char *message)
{
    if (bsp_display_lock(1000)) {
        lv_label_set_text(status_label, message);
        lv_obj_move_foreground(status_label);
        bsp_display_unlock();
    }
}

static const char *center_status_text(void)
{
    switch (voice_visual_state) {
        case VOICE_STATE_LISTENING:
        case VOICE_STATE_THINKING:
        case VOICE_STATE_SPEAKING:
            return voice_status;
        default:
            return "";
    }
}

static void wifi_panel_refresh_locked(bool open)
{
    if (!wifi_panel || !wifi_list || !wifi_info_label) return;
    char ssid[33] = "", ip[16] = "";
    bool connected = voice_client_wifi_info(ssid, sizeof(ssid), ip, sizeof(ip));
    char info[128];
    if (connected) snprintf(info, sizeof(info), "Connected: %.32s\nIP: %.15s", ssid, ip);
    else snprintf(info, sizeof(info), "%s", voice_status[0] ? voice_status : "Not connected. Choose a hotspot below.");
    lv_label_set_text(wifi_info_label, info);

    voice_wifi_network_t networks[VOICE_WIFI_MAX_NETWORKS];
    size_t count = voice_client_wifi_networks(networks, VOICE_WIFI_MAX_NETWORKS);
    lv_obj_clean(wifi_list);
    if (!count) {
        lv_obj_t *empty = lv_label_create(wifi_list);
        lv_label_set_text(empty, "Scanning nearby Wi-Fi...");
        lv_obj_set_style_text_color(empty, lv_color_hex(0xA9B9B1), 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, 18, 0);
    }
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *button = lv_button_create(wifi_list);
        lv_obj_set_size(button, LV_PCT(100), 48);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x263A34), 0);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_add_event_cb(button, wifi_network_button_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *label = lv_label_create(button);
        char text[64];
        snprintf(text, sizeof(text), "%.32s\n%d dBm%s", networks[i].ssid, networks[i].rssi,
                 networks[i].secure ? "  [LOCK]" : "  [OPEN]");
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }
    if (open) {
        lv_obj_remove_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(wifi_panel);
    }
}

static void wifi_panel_refresh(bool open)
{
    if (bsp_display_lock(1000)) {
        wifi_panel_refresh_locked(open);
        bsp_display_unlock();
    }
}

static void wifi_connect_selected(void)
{
    const char *password = lv_textarea_get_text(wifi_password_input);
    esp_err_t err = voice_client_wifi_connect(selected_wifi_ssid, password);
    lv_obj_add_flag(wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    if (err != ESP_OK) {
        char message[80];
        snprintf(message, sizeof(message), "Could not connect: %s", esp_err_to_name(err));
        lv_label_set_text(wifi_info_label, message);
    } else {
        lv_label_set_text(wifi_info_label, "Connecting...\nPlease wait");
    }
}

static void wifi_keyboard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) wifi_connect_selected();
    else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_network_button_cb(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    voice_wifi_network_t networks[VOICE_WIFI_MAX_NETWORKS];
    size_t count = voice_client_wifi_networks(networks, VOICE_WIFI_MAX_NETWORKS);
    if (index >= count) return;
    strlcpy(selected_wifi_ssid, networks[index].ssid, sizeof(selected_wifi_ssid));
    if (!networks[index].secure) {
        lv_textarea_set_text(wifi_password_input, "");
        wifi_connect_selected();
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "Password for\n%.32s", selected_wifi_ssid);
    lv_label_set_text(wifi_password_title, title);
    lv_textarea_set_text(wifi_password_input, "");
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_password_dialog);
    lv_obj_send_event(wifi_password_input, LV_EVENT_FOCUSED, NULL);
}

static void wifi_scan_cb(lv_event_t *event)
{
    (void)event;
    if (voice_client_wifi_scan() == ESP_OK) lv_label_set_text(wifi_info_label, "Scanning nearby Wi-Fi...");
}

static void wifi_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_gesture_cb(lv_event_t *event)
{
    log_touch("GESTURE", event);
    lv_indev_t *input = lv_indev_active();
    if (!input) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_BOTTOM) {
        wifi_panel_refresh_locked(true);
        voice_client_wifi_scan();
    } else if (direction == LV_DIR_TOP && !lv_obj_has_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_wifi_ui(lv_obj_t *screen)
{
    wifi_panel = lv_obj_create(screen);
    lv_obj_set_size(wifi_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_align(wifi_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_panel, lv_color_hex(0x101B18), 0);
    lv_obj_set_style_bg_opa(wifi_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_panel, 0, 0);
    lv_obj_set_style_radius(wifi_panel, 0, 0);
    lv_obj_set_style_pad_all(wifi_panel, 14, 0);
    lv_obj_set_style_pad_row(wifi_panel, 9, 0);
    lv_obj_set_flex_flow(wifi_panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(wifi_panel);
    lv_obj_set_size(header, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_t *actions = lv_obj_create(header);
    lv_obj_set_size(actions, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 7, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_t *scan = lv_button_create(actions);
    lv_obj_set_size(scan, 88, 36);
    lv_obj_add_event_cb(scan, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_label = lv_label_create(scan);
    lv_label_set_text(scan_label, "Refresh");
    lv_obj_center(scan_label);
    lv_obj_t *close = lv_button_create(actions);
    lv_obj_set_size(close, 70, 36);
    lv_obj_add_event_cb(close, wifi_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    wifi_info_label = lv_label_create(wifi_panel);
    lv_obj_set_width(wifi_info_label, LV_PCT(100));
    lv_obj_set_style_text_color(wifi_info_label, lv_color_hex(0xB9D8C9), 0);
    lv_label_set_text(wifi_info_label, "Wi-Fi is starting...");

    wifi_list = lv_obj_create(wifi_panel);
    lv_obj_set_width(wifi_list, LV_PCT(100));
    lv_obj_set_flex_grow(wifi_list, 1);
    lv_obj_set_style_bg_color(wifi_list, lv_color_hex(0x182824), 0);
    lv_obj_set_style_border_width(wifi_list, 0, 0);
    lv_obj_set_style_pad_all(wifi_list, 8, 0);
    lv_obj_set_style_pad_row(wifi_list, 7, 0);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);

    wifi_password_dialog = lv_obj_create(screen);
    lv_obj_set_size(wifi_password_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_password_dialog, lv_color_hex(0x101B18), 0);
    lv_obj_set_style_bg_opa(wifi_password_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_password_dialog, 0, 0);
    lv_obj_set_style_radius(wifi_password_dialog, 0, 0);
    lv_obj_set_style_pad_all(wifi_password_dialog, 12, 0);
    lv_obj_set_style_pad_row(wifi_password_dialog, 8, 0);
    lv_obj_set_flex_flow(wifi_password_dialog, LV_FLEX_FLOW_COLUMN);
    wifi_password_title = lv_label_create(wifi_password_dialog);
    lv_obj_set_width(wifi_password_title, LV_PCT(100));
    lv_obj_set_style_text_align(wifi_password_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(wifi_password_title, lv_color_white(), 0);
    wifi_password_input = lv_textarea_create(wifi_password_dialog);
    lv_obj_set_size(wifi_password_input, LV_PCT(100), 48);
    lv_textarea_set_one_line(wifi_password_input, true);
    lv_textarea_set_password_mode(wifi_password_input, true);
    lv_textarea_set_max_length(wifi_password_input, 64);
    lv_textarea_set_placeholder_text(wifi_password_input, "Hotspot password");
    lv_obj_t *keyboard = lv_keyboard_create(wifi_password_dialog);
    lv_obj_set_width(keyboard, LV_PCT(100));
    lv_obj_set_flex_grow(keyboard, 1);
    lv_keyboard_set_textarea(keyboard, wifi_password_input);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(keyboard, wifi_keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, wifi_keyboard_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void voice_state_changed(voice_state_t state, const char *message)
{
    if (state == VOICE_STATE_WAKE) {
        send_command(CMD_WAKE);
        return;
    }
    voice_visual_state = state;
    if (message && message[0]) strlcpy(voice_status, message, sizeof(voice_status));
    else voice_status[0] = '\0';
    if (state == VOICE_STATE_WIFI_CONNECTED) {
        if (bsp_display_lock(1000)) {
            wifi_panel_refresh_locked(false);
            lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(status_label, "");
            bsp_display_unlock();
        }
        return;
    }
    if (state == VOICE_STATE_WIFI_SCANNING || state == VOICE_STATE_WIFI_SCAN_READY) {
        wifi_panel_refresh(false);
        if (bsp_display_lock(1000)) {
            lv_label_set_text(status_label, "");
            bsp_display_unlock();
        }
        return;
    }
    if (state == VOICE_STATE_WIFI_FAILED) {
        wifi_panel_refresh(true);
        if (bsp_display_lock(1000)) {
            lv_label_set_text(status_label, "");
            bsp_display_unlock();
        }
        return;
    }
    command_t cmd;
    switch (state) {
        case VOICE_STATE_READY:
            cmd = CMD_VOICE_READY;
            if (bsp_display_lock(1000)) {
                lv_label_set_text(status_label, "");
                bsp_display_unlock();
            }
            break;
        case VOICE_STATE_LISTENING: cmd = CMD_VOICE_LISTENING; break;
        case VOICE_STATE_THINKING: cmd = CMD_VOICE_THINKING; break;
        case VOICE_STATE_SPEAKING: cmd = CMD_VOICE_SPEAKING; break;
        default:
            show_status(voice_status);
            return;
    }
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

static void button_task(void *arg)
{
    (void)arg;
    int raw_last = gpio_get_level(BOOT_PIN), stable = raw_last;
    int64_t changed = esp_timer_get_time(), pressed = changed;
    bool armed = stable == 1, held = false;
    for (;;) {
        int raw = gpio_get_level(BOOT_PIN);
        int64_t now = esp_timer_get_time();
        if (raw != raw_last) { raw_last = raw; changed = now; }
        if (raw != stable && now - changed >= DEBOUNCE_MS * 1000LL) {
            stable = raw;
            if (!stable) {
                pressed = now;
                held = false;
                if (voice_client_enabled() && armed) voice_client_begin_turn();
            }
            else {
                if (voice_client_enabled()) {
                    if (armed) voice_client_end_turn();
                } else if (armed && !held) send_command(CMD_NEXT);
                armed = true; /* Ignore a key already held at startup. */
            }
        }
        if (!voice_client_enabled() && armed && !stable && !held && now - pressed >= HOLD_MS * 1000LL) {
            held = true;
            send_command(CMD_HOME);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* The JPEG decoder handle and input buffer are shared between the playback
 * fallback path and the background cache task; the lock covers read + decode. */
static bool decode_jpeg(unsigned action, unsigned frame, uint16_t *out)
{
    char path[80];
    snprintf(path, sizeof(path), "/storage/%s_%02u.jpg", anim_names[action], frame);
    /* Break the cost into lock wait / flash read / decode so an action-switch
     * stall can be attributed instead of guessed at. Only logged when the
     * total exceeds the 30 ms Phase 0 budget, to keep steady state quiet. */
    int64_t t_start = esp_timer_get_time();
    if (xSemaphoreTake(decoder_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    int64_t t_lock = esp_timer_get_time();
    int64_t t_open = t_lock;
    int64_t t_read = t_lock;
    size_t size = 0;   /* declared before any goto so the cost log is always defined */
    bool ok = false;
    /* POSIX open/read rather than fopen/fread: picolibc lazily allocates a
     * recursive FILE lock on first fread, and lock_init_generic() calls abort()
     * when that allocation fails. Under internal-RAM pressure that turned a
     * recoverable decode fallback into a reboot. read() has no such path. */
    int fd = open(path, O_RDONLY);
    t_open = esp_timer_get_time();
    if (fd < 0) { ESP_LOGE(TAG, "Missing %s", path); goto out; }
    while (size < JPEG_CAPACITY) {
        ssize_t got = read(fd, jpeg_bytes + size, JPEG_CAPACITY - size);
        if (got < 0) { size = 0; break; }
        if (got == 0) break;
        size += (size_t)got;
    }
    close(fd);
    t_read = esp_timer_get_time();
    bool bad = size == 0 || size == JPEG_CAPACITY;
    if (bad) { ESP_LOGE(TAG, "Invalid/oversize JPEG: %s", path); goto out; }

    jpeg_dec_io_t io = { .inbuf = jpeg_bytes, .inbuf_len = (int)size };
    jpeg_dec_header_info_t header = {0};
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
        header.width != ANIM_WIDTH || header.height != ANIM_HEIGHT) {
        ESP_LOGE(TAG, "Bad dimensions/JPEG header: %s", path);
        goto out;
    }
    int output_length = 0;
    jpeg_dec_get_outbuf_len(decoder, &output_length);
    if (output_length <= 0 || output_length > FRAME_BYTES) goto out;
    io.outbuf = (uint8_t *)out;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Decode failed: %s", path);
        goto out;
    }
    ok = true;
out:;
    int64_t t_end = esp_timer_get_time();
    xSemaphoreGive(decoder_lock);
    atomic_fetch_add(&decode_cost_us, (int)(t_end - t_start));
    atomic_fetch_add(&decode_cost_frames, 1);
    /* Only frame 0 gets the detailed breakdown: it is the one that determines
     * how long an action switch takes to show anything new. */
    if (frame == 0 && t_end - t_start > 30000)
        ESP_LOGW(TAG, "Decode cost %s_00: lock=%lld open=%lld read=%lld decode=%lld total=%lld us bytes=%u",
                 anim_names[action], (long long)(t_lock - t_start),
                 (long long)(t_open - t_lock), (long long)(t_read - t_open),
                 (long long)(t_end - t_read), (long long)(t_end - t_start), (unsigned)size);
    return ok;
}

static bool display_frame(unsigned action, unsigned frame)
{
    /* Mark the canvas as "possibly showing a shared buffer" before inspecting the
     * cache state; the cache task waits for this flag to clear before rewriting
     * cache buffers, so a bound buffer is never decoded into. With the staging
     * path below this flag is held for well under a millisecond, never across
     * flash I/O. */
    atomic_store(&cache_busy, true);
    uint16_t *pixels;
    bool staging = false;
    if (cache_enabled) {
        if (atomic_load(&cache_action) == (int)action &&
            (atomic_load(&cache_mask) & (1u << frame))) {
            pixels = frame_cache[frame];
        } else if (atomic_load(&stage_ready)) {
            /* New action, cache still warming. Show the frame the cache task
             * staged rather than decoding here: an inline decode costs ~170 ms
             * (SPIFFS object-index scan + JPEG), which both overruns wave's
             * 150 ms hold and starves IDLE0 past the 5 s task watchdog. */
            pixels = scratch_pixels[0];
            staging = true;
        } else {
            atomic_store(&cache_busy, false);
            return true;   /* hold the current frame; nothing safe to bind yet */
        }
    } else {
        scratch_index ^= 1;
        if (!decode_jpeg(action, frame, scratch_pixels[1])) {
            atomic_store(&cache_busy, false);
            return false;
        }
        pixels = scratch_pixels[1];
    }
    int64_t t_bind = esp_timer_get_time();
    if (!bsp_display_lock(1000)) {
        atomic_store(&cache_busy, false);
        return false;
    }
    int64_t t_lock = esp_timer_get_time();
    lv_canvas_set_buffer(canvas, pixels, ANIM_WIDTH, ANIM_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);
    lv_label_set_text(status_label, center_status_text());
    bsp_display_unlock();
    /* Publish the binding before releasing cache_busy so the cache task can
     * never observe a stale "not bound" state for a buffer now on screen. */
    atomic_store(&stage_bound, staging);
    atomic_store(&bound_frame, staging ? -1 : (int)frame);
    atomic_store(&cache_busy, false);
    int64_t t_done = esp_timer_get_time();
    /* The remaining switch-time overrun is no longer decode; attribute it to
     * waiting for the LVGL task versus the bind itself. */
    if (t_done - t_bind > 30000)
        ESP_LOGW(TAG, "Display cost %s_%02u: lvgl_lock=%lld bind=%lld total=%lld us",
                 anim_names[action], frame, (long long)(t_lock - t_bind),
                 (long long)(t_done - t_lock), (long long)(t_done - t_bind));
    return true;
}

/* Wait until playback is no longer showing any cache buffer. Once it has moved
 * to the staging buffer, cache slot i can only be bound after its mask bit is
 * set, and that bit is set only after the decode into slot i has finished. */
static bool wait_cache_unbound(int64_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while ((atomic_load(&cache_busy) || atomic_load(&bound_frame) >= 0) &&
           esp_timer_get_time() - start < timeout_us)
        vTaskDelay(pdMS_TO_TICKS(5));
    return atomic_load(&bound_frame) < 0 && !atomic_load(&cache_busy);
}

static void decode_task(void *arg)
{
    (void)arg;
    for (;;) {
        int want = atomic_load(&decode_request);
        if (want == atomic_load(&cache_action) && atomic_load(&cache_mask) == CACHE_FULL_MASK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        atomic_store(&cache_action, -1);
        atomic_store(&cache_mask, 0);
        /* Stage frame 0 of the new action first so playback has something to
         * show within one decode instead of freezing for the whole 8-frame
         * warm-up. Skipped when the staging buffer is itself on screen. */
        if (!atomic_load(&stage_bound) && !atomic_load(&cache_busy)) {
            if (decode_jpeg((unsigned)want, 0, scratch_pixels[0])) {
                atomic_store(&stage_action, want);
                atomic_store(&stage_ready, true);
            }
        }
        if (!wait_cache_unbound(2000000))
            ESP_LOGW(TAG, "Cache still bound after 2 s; rewriting anyway");
        atomic_store(&cache_action, want);
        bool ok = true;
        for (unsigned frame = 0; frame < ANIM_FRAMES; frame++) {
            if (!decode_jpeg((unsigned)want, frame, frame_cache[frame])) { ok = false; break; }
            atomic_fetch_or(&cache_mask, 1u << frame);
        }
        if (ok) {
            int total = atomic_exchange(&decode_cost_us, 0);
            int frames = atomic_exchange(&decode_cost_frames, 0);
            ESP_LOGI(TAG, "Cached %s (%u frames in PSRAM, decode total=%d ms avg=%d ms)",
                     anim_names[want], ANIM_FRAMES, total / 1000,
                     frames ? total / 1000 / frames : 0);
        } else {
            atomic_store(&decode_cost_us, 0);
            atomic_store(&decode_cost_frames, 0);
            atomic_store(&cache_action, -1);
            atomic_store(&cache_mask, 0);
            ESP_LOGW(TAG, "Cache decode failed for %s; retrying", anim_names[want]);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    unsigned action = 0, frame = 0;
    bool wake_response = false;
    int64_t deadline = esp_timer_get_time();
    /* Unconditional render-cost statistics. "Frame slow" only fires when a frame
     * overruns its hold time, so it cannot prove the <30 ms acceptance target;
     * these counters measure every frame and report once per FRAME_STATS_PERIOD. */
    uint32_t stat_count = 0;
    int64_t stat_min = INT64_MAX, stat_max = 0, stat_sum = 0;
    uint32_t stat_overrun = 0;
    for (;;) {
        int64_t started = esp_timer_get_time();
        atomic_store(&decode_request, (int)action);
        bool ok = display_frame(action, frame);
        uint32_t hold_ms = ok ? anim_durations[action][frame] : 1000;
        if (!ok) show_status("Asset error\nSee USB serial log");
        deadline += hold_ms * 1000LL;
        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - started;
        if (ok) {
            stat_count++;
            stat_sum += elapsed;
            if (elapsed < stat_min) stat_min = elapsed;
            if (elapsed > stat_max) stat_max = elapsed;
            if (elapsed > (int64_t)hold_ms * 1000LL) stat_overrun++;
            if (stat_count >= FRAME_STATS_PERIOD) {
                ESP_LOGI(TAG, "Frame stats: n=%u min=%lld avg=%lld max=%lld us overrun=%u cache=%s",
                         (unsigned)stat_count, (long long)stat_min,
                         (long long)(stat_sum / stat_count), (long long)stat_max,
                         (unsigned)stat_overrun, cache_enabled ? "on" : "off");
                stat_count = 0; stat_sum = 0; stat_overrun = 0;
                stat_min = INT64_MAX; stat_max = 0;
                /* Periodic heap trend on core 0 so a 10-minute soak can be
                 * checked for leaks and for DMA-block fragmentation drift. */
                ESP_LOGI(TAG, "Heap trend: internal=%u dma_block=%u psram=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
        }
        if (deadline <= now) deadline = now + 1000;
        command_t cmd;
        TickType_t wait = pdMS_TO_TICKS((deadline - now + 999) / 1000);
        if (xQueueReceive(commands, &cmd, wait ? wait : 1) == pdTRUE) {
            do {
                if (cmd == CMD_VOICE_READY && wake_response) continue;
                wake_response = cmd == CMD_WAKE || cmd == CMD_ACT_WAVE || cmd == CMD_ACT_JUMP;
                if (cmd == CMD_HOME || cmd == CMD_VOICE_READY || cmd == CMD_VOICE_LISTENING) action = 0;
                else if (cmd == CMD_VOICE_THINKING || cmd == CMD_ACT_SLEEP) action = 3;
                else if (cmd == CMD_VOICE_SPEAKING || cmd == CMD_WAKE || cmd == CMD_ACT_WAVE) action = 1;
                else if (cmd == CMD_ACT_JUMP) action = 2;
                else action = (action + 1) % ANIM_COUNT;
            } while (xQueueReceive(commands, &cmd, 0) == pdTRUE);
            frame = 0;
            deadline = esp_timer_get_time();
            ESP_LOGI(TAG, "Selected %s", anim_names[action]);
        } else {
            frame = (frame + 1) % ANIM_FRAMES;
            if (wake_response && frame == 0) {
                action = 0;
                wake_response = false;
                ESP_LOGI(TAG, "Wake response complete; idle");
            }
        }
        if (ok && now - started > hold_ms * 1000LL)
            ESP_LOGW(TAG, "Frame slow: %lld us", (long long)(now - started));
    }
}

/* Reproducible board-side test channel. Action switches, wake responses and
 * cache warm-up previously could only be exercised by a human tapping the
 * screen or speaking, which made the Phase 0 acceptance runs non-repeatable and
 * mixed real defects with accidental long-press config wipes. Reading short
 * line commands over USB-Serial/JTAG drives the exact same command queue as
 * physical input, so a scripted soak produces identical stimulus every run.
 * This does NOT verify speech recognition: "wake" here only injects CMD_WAKE. */
static void console_task(void *arg)
{
    (void)arg;
    /* CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y, so esp_stdio already installed this
     * driver at system init (usb_serial_jtag_vfs.c: usb_serial_jtag_vfs_dev_port_init).
     * Installing again returns ESP_ERR_INVALID_STATE and prints an error line into
     * every boot log, so reuse the existing instance and only install if absent. */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Test console unavailable: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "Test console ready (next/idle/wave/jump/sleep/wake)");
    char line[32];
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
        command_t cmd;
        if (!strcmp(line, "next")) cmd = CMD_NEXT;
        else if (!strcmp(line, "idle") || !strcmp(line, "home")) cmd = CMD_HOME;
        else if (!strcmp(line, "wave")) cmd = CMD_ACT_WAVE;
        else if (!strcmp(line, "jump")) cmd = CMD_ACT_JUMP;
        else if (!strcmp(line, "sleep")) cmd = CMD_ACT_SLEEP;
        else if (!strcmp(line, "wake")) cmd = CMD_WAKE;
        else if (!line[0]) continue;
        else { ESP_LOGW(TAG, "Unknown test command: %s", line); continue; }
        ESP_LOGI(TAG, "Test command: %s", line);
        send_command(cmd);
    }
}

void app_main(void)
{
    commands = xQueueCreate(8, sizeof(command_t));
    if (!commands) { ESP_LOGE(TAG, "Queue allocation failed"); return; }
    bsp_display_cfg_t cfg = { .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG() };
    /* Keep display work off core 1 so speech inference owns that core. */
    cfg.lvgl_port_cfg.task_affinity = 0;
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (!display) { ESP_LOGE(TAG, "Display start failed"); return; }
    /* Phase 0.4 DMA governance. The BSP allocates LVGL's draw buffer with
     * MALLOC_CAP_DEFAULT; at 410 * 20 * 2 = 16400 B it exceeds
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16384) and therefore lands in PSRAM.
     * Every QSPI flush then makes spi_common allocate a temporary 16400 B
     * internal DMA "priv TX buffer". While MultiNet7 is created that block does
     * not exist: 202 consecutive flushes failed inside a 1.6 s window
     * (p0-verify-600s.log, 416354-417953 ms) and the panel flickered. Rebind the
     * draw buffer to internal DMA RAM reserved once at boot so no per-flush
     * allocation is needed and model loading can no longer starve the display.
     * esp_lvgl_port only keeps its own pointer for deinit, and the flush path
     * uses the buffer LVGL hands it, so replacing it here is safe. */
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
    /* A long press wipes the Wi-Fi/device configuration and reboots. LVGL's
     * default 400 ms threshold is short enough that an ordinary tap can be
     * reported as a long press when the LVGL task is busy rendering a full
     * canvas refresh: three accidental RTC_SW_CPU_RST reboots were captured
     * during a tap sequence (p0-dma-630s.log). Require a deliberate 3 s hold so
     * the destructive action cannot be triggered by normal interaction. */
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch) lv_indev_set_long_press_time(touch, 3000);
    scratch_pixels[0] = heap_caps_aligned_calloc(16, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    scratch_pixels[1] = heap_caps_aligned_calloc(16, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_bytes = heap_caps_malloc(JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch_pixels[0] || !scratch_pixels[1] || !jpeg_bytes) {
        ESP_LOGE(TAG, "PSRAM buffer allocation failed"); return;
    }
    decoder_lock = xSemaphoreCreateMutex();
    if (!decoder_lock) { ESP_LOGE(TAG, "Decoder lock allocation failed"); return; }
    cache_enabled = true;
    for (unsigned frame = 0; frame < ANIM_FRAMES; frame++) {
        frame_cache[frame] = heap_caps_aligned_calloc(16, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame_cache[frame]) { cache_enabled = false; break; }
    }
    if (!cache_enabled) {
        for (unsigned frame = 0; frame < ANIM_FRAMES; frame++) {
            heap_caps_free(frame_cache[frame]);
            frame_cache[frame] = NULL;
        }
        ESP_LOGW(TAG, "Frame cache allocation failed; decode-on-demand only");
    }
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, scratch_pixels[0], ANIM_WIDTH, ANIM_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(canvas, setup_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(canvas, wifi_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(screen, setup_hold_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(screen, wifi_gesture_cb, LV_EVENT_GESTURE, NULL);
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(status_label);
    lv_label_set_text(status_label, "Loading...");
    create_wifi_ui(screen);
    bsp_display_unlock();
    esp_vfs_spiffs_conf_t fs = {
        .base_path = "/storage", .partition_label = "storage", .max_files = 2,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS: %s", esp_err_to_name(err));
        show_status("Missing assets\nFlash storage image too");
        return;
    }
    jpeg_dec_config_t jpeg_cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&jpeg_cfg, &decoder) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG decoder failed"); show_status("JPEG init error"); return;
    }
    if (cache_enabled) {
        bool ok = true;
        for (unsigned frame = 0; frame < ANIM_FRAMES; frame++)
            if (!decode_jpeg(0, frame, frame_cache[frame])) { ok = false; break; }
        if (ok) {
            atomic_store(&cache_mask, CACHE_FULL_MASK);
            atomic_store(&cache_action, 0);
            /* dma_block is the largest contiguous DMA-capable internal block.
             * Total free internal bytes can look healthy while fragmentation
             * still starves the SPI DMA allocator, which is what caused the
             * boot flicker; report both so the two are never confused. */
            ESP_LOGI(TAG, "Idle pre-decoded; free internal=%u dma_block=%u PSRAM=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "Idle pre-decode failed; decode-on-demand only");
        }
    }
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOOT_PIN, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    if (xTaskCreate(button_task, "button", 3072, NULL, 4, NULL) != pdPASS ||
        xTaskCreatePinnedToCore(playback_task, "animation", 8192, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed"); show_status("Task init error");
    }
    if (cache_enabled &&
        xTaskCreatePinnedToCore(decode_task, "frame_cache", 4096, NULL, 3, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "Cache task creation failed; idle cache only");
    /* Low priority, unpinned: the test channel must never compete with the
     * playback task on core 0 or speech inference on core 1. */
    if (xTaskCreate(console_task, "test_console", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW(TAG, "Test console task creation failed");
    if (voice_client_enabled()) {
        esp_err_t voice_err = voice_client_start(voice_state_changed, voice_action_changed);
        if (voice_err != ESP_OK) {
            ESP_LOGE(TAG, "Voice init failed: %s", esp_err_to_name(voice_err));
            show_status("Voice init failed");
        }
    }
}
