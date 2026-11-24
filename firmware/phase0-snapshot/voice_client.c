#include "sdkconfig.h"
#include "voice_client.h"

#if CONFIG_WHALE_VOICE_ENABLED

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#define AUDIO_RATE 16000
#define WAKE_WORD "Hi ESP"
#define WAKE_COOLDOWN_US 2500000LL
#define WAKE_ECHO_GUARD_MS 500
#define COMMAND_WINDOW_MS 6000
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAX_RETRIES 8

typedef struct {
    bool chime;
    size_t length;
    uint8_t data[];
} audio_chunk_t;

typedef struct {
    char ssid[33];
    char password[65];
    char bridge_uri[256];
    char token[129];
} runtime_config_t;

/* action indexes match anim_names[] in animation_manifest.h */
static const struct {
    int id;
    const char *pinyin;
    unsigned action;
} voice_commands[] = {
    { 1, "hui shou",    1 },
    { 2, "tiao yi xia", 2 },
    { 3, "shui jiao",   3 },
};

static const char *TAG = "voice_client";
static EventGroupHandle_t wifi_events;
static QueueHandle_t speaker_queue;
static SemaphoreHandle_t recording_lock;
static esp_websocket_client_handle_t websocket;
static esp_codec_dev_handle_t microphone;
static esp_codec_dev_handle_t speaker;
static voice_state_callback_t state_callback;
static voice_action_callback_t action_callback;
static atomic_bool recording;
static atomic_bool socket_ready;
static atomic_bool remote_turn_active;
static atomic_bool speech_ready;
static atomic_bool audio_ready;
static int16_t *speech_feed_pcm;
static int speech_feed_samples;
static atomic_uint speaker_pending;
/* Phase 1.2 playback watermark. The bridge paces one 1280-byte frame (40 ms of
 * 16 kHz mono PCM) every 35 ms, so it produces slightly faster than the codec
 * consumes and the queue drifts upward over a long reply. Measuring bytes, not
 * slots, is what makes that drift visible and bounded. */
#define SPEAKER_QUEUE_SLOTS      192   /* ~245 KB of PCM in PSRAM, ~7.7 s of audio */
#define SPEAKER_HIGH_WATER_BYTES 122880  /* ~3.8 s buffered: log the crossing */
#define SPEAKER_LOW_WATER_BYTES  30720   /* ~0.96 s: hysteresis reset point */
static atomic_uint speaker_bytes;
static atomic_uint speaker_dropped;
static atomic_bool speaker_high;
static atomic_uint reconnect_count;
static _Atomic TickType_t last_speaker_tick;
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static const esp_mn_iface_t *multinet;
static model_iface_data_t *command_model;
static srmodel_list_t *speech_models;
static int wifi_retries;
static runtime_config_t runtime_config;
static bool resetting_config;
static bool provisioning_mode;
static bool wifi_driver_ready;
static bool wifi_connected;
static bool scan_in_progress;
static char connected_ssid[33];
static char connected_ip[16];
static voice_wifi_network_t wifi_networks[VOICE_WIFI_MAX_NETWORKS];
static size_t wifi_network_count;
static portMUX_TYPE wifi_data_lock = portMUX_INITIALIZER_UNLOCKED;

static const char provisioning_page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Qiban setup</title><style>body{font:16px system-ui;max-width:520px;margin:35px auto;padding:18px}"
    "input{box-sizing:border-box;width:100%;padding:12px;margin:6px 0 16px;border:1px solid #aaa;border-radius:8px}"
    "button{padding:12px 20px;border:0;border-radius:8px;background:#2b7655;color:white;font-weight:700}</style>"
    "<h1>Qiban voice setup</h1><p>Enter the 2.4 GHz Wi-Fi and voice bridge settings.</p>"
    "<form method=post action=/save><label>Wi-Fi name</label><input name=ssid maxlength=32 required>"
    "<label>Wi-Fi password</label><input name=password type=password maxlength=64>"
    "<label>Voice bridge WebSocket URL</label><input name=bridge placeholder='wss://example.com/voice' maxlength=255 required>"
    "<label>Device token</label><input name=token type=password maxlength=128 required>"
    "<button>Save and restart</button></form>";

static void report(voice_state_t state, const char *message)
{
    if (state_callback) state_callback(state, message);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (runtime_config.ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = 24;
        wifi_ap_record_t *records = calloc(count, sizeof(*records));
        size_t saved = 0;
        if (records && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            portENTER_CRITICAL(&wifi_data_lock);
            memset(wifi_networks, 0, sizeof(wifi_networks));
            for (uint16_t i = 0; i < count && saved < VOICE_WIFI_MAX_NETWORKS; i++) {
                const char *ssid = (const char *)records[i].ssid;
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (size_t j = 0; j < saved; j++)
                    if (!strcmp(wifi_networks[j].ssid, ssid)) { duplicate = true; break; }
                if (duplicate) continue;
                strlcpy(wifi_networks[saved].ssid, ssid, sizeof(wifi_networks[saved].ssid));
                wifi_networks[saved].rssi = records[i].rssi;
                wifi_networks[saved].secure = records[i].authmode != WIFI_AUTH_OPEN;
                saved++;
            }
            wifi_network_count = saved;
            scan_in_progress = false;
            portEXIT_CRITICAL(&wifi_data_lock);
        } else {
            portENTER_CRITICAL(&wifi_data_lock);
            scan_in_progress = false;
            portEXIT_CRITICAL(&wifi_data_lock);
        }
        free(records);
        ESP_LOGI(TAG, "Wi-Fi scan found %u network(s)", (unsigned)saved);
        report(VOICE_STATE_WIFI_SCAN_READY, saved ? "Choose a Wi-Fi network" : "No Wi-Fi networks found");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        wifi_connected = false;
        if (wifi_retries++ < WIFI_MAX_RETRIES) {
            report(VOICE_STATE_CONNECTING, "Wi-Fi reconnecting...");
            esp_wifi_connect();
        } else {
            if (provisioning_mode) {
                wifi_retries = 0;
                report(VOICE_STATE_WIFI_FAILED, "Wi-Fi failed. Check password and retry.");
                voice_client_wifi_scan();
            } else {
                xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
                report(VOICE_STATE_ERROR, "Wi-Fi connection failed");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        wifi_retries = 0;
        portENTER_CRITICAL(&wifi_data_lock);
        wifi_connected = true;
        strlcpy(connected_ssid, runtime_config.ssid, sizeof(connected_ssid));
        snprintf(connected_ip, sizeof(connected_ip), IPSTR, IP2STR(&event->ip_info.ip));
        portEXIT_CRITICAL(&wifi_data_lock);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        report(VOICE_STATE_WIFI_CONNECTED, "Wi-Fi connected");
        if (provisioning_mode) voice_client_wifi_scan();
    }
}

static esp_err_t network_stack_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "Wi-Fi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL), TAG, "IP handler");
    return ESP_OK;
}

static esp_err_t load_runtime_config(runtime_config_t *config)
{
    memset(config, 0, sizeof(*config));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("qiban_voice", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t ssid = sizeof(config->ssid), password = sizeof(config->password);
        size_t bridge = sizeof(config->bridge_uri), token = sizeof(config->token);
        esp_err_t have_ssid = nvs_get_str(handle, "ssid", config->ssid, &ssid);
        nvs_get_str(handle, "password", config->password, &password);
        esp_err_t have_bridge = nvs_get_str(handle, "bridge", config->bridge_uri, &bridge);
        esp_err_t have_token = nvs_get_str(handle, "token", config->token, &token);
        nvs_close(handle);
        if (have_ssid == ESP_OK && config->ssid[0]) {
            if (have_bridge == ESP_OK && have_token == ESP_OK && config->bridge_uri[0] && config->token[0]) return ESP_OK;
            return ESP_ERR_NOT_FINISHED;
        }
    }
    if (CONFIG_WHALE_WIFI_SSID[0] && CONFIG_WHALE_VOICE_BRIDGE_URI[0] && CONFIG_WHALE_VOICE_BRIDGE_TOKEN[0]) {
        strlcpy(config->ssid, CONFIG_WHALE_WIFI_SSID, sizeof(config->ssid));
        strlcpy(config->password, CONFIG_WHALE_WIFI_PASSWORD, sizeof(config->password));
        strlcpy(config->bridge_uri, CONFIG_WHALE_VOICE_BRIDGE_URI, sizeof(config->bridge_uri));
        strlcpy(config->token, CONFIG_WHALE_VOICE_BRIDGE_TOKEN, sizeof(config->token));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void url_decode(char *output, size_t capacity, const char *input, size_t length)
{
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < capacity; i++) {
        if (input[i] == '+') output[used++] = ' ';
        else if (input[i] == '%' && i + 2 < length && isxdigit((unsigned char)input[i + 1]) && isxdigit((unsigned char)input[i + 2])) {
            char hex[3] = { input[i + 1], input[i + 2], 0 };
            output[used++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else output[used++] = input[i];
    }
    output[used] = '\0';
}

static bool form_value(const char *body, const char *name, char *output, size_t capacity)
{
    size_t name_length = strlen(name);
    const char *cursor = body;
    while (cursor && *cursor) {
        if ((!cursor[0] || cursor == body || cursor[-1] == '&') &&
            !strncmp(cursor, name, name_length) && cursor[name_length] == '=') {
            const char *value = cursor + name_length + 1;
            const char *end = strchr(value, '&');
            url_decode(output, capacity, value, end ? (size_t)(end - value) : strlen(value));
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor) cursor++;
    }
    return false;
}

static esp_err_t save_runtime_config(const runtime_config_t *config)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("qiban_voice", NVS_READWRITE, &handle), TAG, "open voice config");
    esp_err_t err = nvs_set_str(handle, "ssid", config->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", config->password);
    if (err == ESP_OK) err = nvs_set_str(handle, "bridge", config->bridge_uri);
    if (err == ESP_OK) err = nvs_set_str(handle, "token", config->token);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t save_wifi_config(const runtime_config_t *config)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("qiban_voice", NVS_READWRITE, &handle), TAG, "open Wi-Fi config");
    esp_err_t err = nvs_set_str(handle, "ssid", config->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", config->password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t setup_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, provisioning_page, HTTPD_RESP_USE_STRLEN);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t setup_save(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 700) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
        return ESP_FAIL;
    }
    char *body = calloc(1, request->content_len + 1);
    if (!body) return ESP_ERR_NO_MEM;
    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result <= 0) { free(body); return ESP_FAIL; }
        received += result;
    }
    runtime_config_t submitted = {0};
    bool ok = form_value(body, "ssid", submitted.ssid, sizeof(submitted.ssid)) &&
              form_value(body, "password", submitted.password, sizeof(submitted.password)) &&
              form_value(body, "bridge", submitted.bridge_uri, sizeof(submitted.bridge_uri)) &&
              form_value(body, "token", submitted.token, sizeof(submitted.token));
    free(body);
    ok = ok && submitted.ssid[0] && strlen(submitted.token) >= 8 &&
         (!strncmp(submitted.bridge_uri, "ws://", 5) || !strncmp(submitted.bridge_uri, "wss://", 6));
    if (!ok || save_runtime_config(&submitted) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Check SSID, WebSocket URL and token");
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request, "<h1>Saved</h1><p>The device is restarting. You can close this page.</p>");
    xTaskCreate(restart_task, "voice_restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t provisioning_start(const runtime_config_t *saved)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_name[33];
    snprintf(ap_name, sizeof(ap_name), "Qiban-%02X%02X", mac[4], mac[5]);
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "AP Wi-Fi init");
    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, ap_name, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, "qiban1234", sizeof(config.ap.password));
    config.ap.ssid_len = strlen(ap_name);
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "AP+STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
    if (saved && saved->ssid[0]) {
        wifi_config_t station = {0};
        strlcpy((char *)station.sta.ssid, saved->ssid, sizeof(station.sta.ssid));
        strlcpy((char *)station.sta.password, saved->password, sizeof(station.sta.password));
        station.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG, "saved Wi-Fi config");
    }
    provisioning_mode = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");
    wifi_driver_ready = true;
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &http_config), TAG, "setup web server");
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = setup_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = setup_save };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    char status[96];
    snprintf(status, sizeof(status), "Swipe down to choose Wi-Fi\nSetup AP: %s", ap_name);
    report(VOICE_STATE_CONNECTING, status);
    if (!saved || !saved->ssid[0]) voice_client_wifi_scan();
    ESP_LOGI(TAG, "Provisioning AP %s started; open http://192.168.4.1", ap_name);
    return ESP_OK;
}

static esp_err_t wifi_start(const runtime_config_t *runtime)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, runtime->ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, runtime->password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "Wi-Fi config");
    provisioning_mode = false;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    wifi_driver_ready = true;
    return ESP_OK;
}

static esp_err_t audio_start(void)
{
    speaker = bsp_audio_codec_speaker_init();
    microphone = bsp_audio_codec_microphone_init();
    if (!speaker || !microphone) return ESP_FAIL;
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_RATE,
        .channel = 1,
        .channel_mask = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_open(microphone, &format) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_out_vol(speaker, CONFIG_WHALE_SPEAKER_VOLUME);
    esp_codec_dev_set_in_gain(microphone, CONFIG_WHALE_MIC_GAIN_DB);
    return ESP_OK;
}

/* Runs in the WebSocket event task, which esp_websocket_client invokes while
 * holding client->lock. It must never block: doing so would stall microphone
 * sends and the reconnect path. Watermark control here is therefore bounded
 * buffer management plus measurement, not producer backpressure. */
static void enqueue_speaker_chunk(audio_chunk_t *chunk)
{
    atomic_fetch_add(&speaker_pending, 1);
    unsigned bytes = atomic_fetch_add(&speaker_bytes, chunk->length) + chunk->length;
    if (bytes >= SPEAKER_HIGH_WATER_BYTES && !atomic_exchange(&speaker_high, true))
        ESP_LOGW(TAG, "Speaker watermark high: %u bytes (~%u ms) queued",
                 bytes, bytes / 32);
    if (xQueueSend(speaker_queue, &chunk, 0) == pdTRUE) return;

    /* Hard overflow: evict the oldest queued audio so playback keeps moving
     * forward with the newest speech instead of truncating the tail. Counted
     * and logged, never silent. With 192 slots this should not be reachable. */
    audio_chunk_t *old = NULL;
    if (xQueueReceive(speaker_queue, &old, 0) == pdTRUE && old) {
        atomic_fetch_sub(&speaker_bytes, old->length);
        atomic_fetch_sub(&speaker_pending, 1);
        free(old);
        unsigned drops = atomic_fetch_add(&speaker_dropped, 1) + 1;
        ESP_LOGW(TAG, "Speaker buffer full; evicted oldest chunk (drops=%u)", drops);
        if (xQueueSend(speaker_queue, &chunk, 0) == pdTRUE) return;
    }
    atomic_fetch_sub(&speaker_bytes, chunk->length);
    atomic_fetch_sub(&speaker_pending, 1);
    free(chunk);
    ESP_LOGW(TAG, "Speaker queue full");
}

/* Discard audio belonging to a turn that can no longer finish, so a reconnect
 * never replays the previous answer. Safe against speaker_task: FreeRTOS queue
 * operations are atomic and each chunk has exactly one owner. */
static void flush_speaker_queue(void)
{
    audio_chunk_t *chunk = NULL;
    unsigned flushed = 0;
    while (xQueueReceive(speaker_queue, &chunk, 0) == pdTRUE && chunk) {
        atomic_fetch_sub(&speaker_bytes, chunk->length);
        atomic_fetch_sub(&speaker_pending, 1);
        free(chunk);
        flushed++;
    }
    atomic_store(&speaker_high, false);
    if (flushed)
        ESP_LOGI(TAG, "Flushed %u stale speaker chunks on disconnect", flushed);
}

static void queue_speaker_audio(const uint8_t *data, size_t length)
{
    if (!length || !speaker_queue) return;
    /* PSRAM, not internal: with SPIRAM_MALLOC_ALWAYSINTERNAL=16384 a plain
     * malloc() here lands in internal RAM, and a full 24-deep queue of ~1 KB
     * chunks consumes the contiguous DMA block the QSPI display needs. That
     * starved spicommon_dma_setup_priv_buffer and aborted the board mid-turn.
     * esp_codec_dev_write() copies into its own I2S DMA buffer, so the source
     * does not need to be DMA-capable. */
    audio_chunk_t *chunk = heap_caps_malloc(sizeof(*chunk) + length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGW(TAG, "Speaker chunk allocation failed (%u bytes)", (unsigned)length);
        return;
    }
    chunk->length = length;
    chunk->chime = false;
    memcpy(chunk->data, data, length);
    enqueue_speaker_chunk(chunk);
}

static void queue_wake_chime(void)
{
    audio_chunk_t *chunk = heap_caps_calloc(1, sizeof(*chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "Wake chime allocation failed");
        return;
    }
    chunk->chime = true;
    enqueue_speaker_chunk(chunk);
}

static void play_wake_chime(void)
{
    int16_t pcm[320];
    for (int offset = 0; offset < 5760; offset += 320) {
        for (int i = 0; i < 320; i++) {
            int sample = offset + i;
            int position = 0, duration = 0;
            float frequency = 0;
            if (sample < 1920) {
                position = sample;
                duration = 1920;
                frequency = 660;
            } else if (sample >= 2560 && sample < 4800) {
                position = sample - 2560;
                duration = 2240;
                frequency = 880;
            }
            pcm[i] = 0;
            if (duration) {
                float envelope = fminf(1.0f, fminf(position / 160.0f, (duration - 1 - position) / 160.0f));
                pcm[i] = (int16_t)(7000.0f * envelope * sinf(6.2831853f * frequency * position / AUDIO_RATE));
            }
        }
        if (esp_codec_dev_write(speaker, pcm, sizeof(pcm)) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Wake chime playback failed");
            return;
        }
    }
    ESP_LOGI(TAG, "Wake chime played");
}

static void handle_text(const char *data, size_t length)
{
    /* The bridge protocol has a deliberately tiny fixed state vocabulary. Avoid
     * pulling a JSON DOM into the firmware for messages we do not need to retain. */
    char message[384];
    size_t used = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
    memcpy(message, data, used);
    message[used] = '\0';
    if (!strstr(message, "\"type\":\"state\"")) return;
    if (strstr(message, "\"state\":\"ready\"") || strstr(message, "\"state\":\"idle\"")) {
        socket_ready = true;
        recording = false;
        remote_turn_active = false;
        report(VOICE_STATE_READY, "Say Hi ESP, or hold BOOT to talk");
    } else if (strstr(message, "\"state\":\"listening\"")) {
        report(VOICE_STATE_LISTENING, "Listening... release to send");
    } else if (strstr(message, "\"state\":\"thinking\"")) {
        report(VOICE_STATE_THINKING, "Thinking...");
    } else if (strstr(message, "\"state\":\"speaking\"")) {
        report(VOICE_STATE_SPEAKING, "Speaking...");
    } else if (strstr(message, "\"state\":\"error\"")) {
        recording = false;
        remote_turn_active = false;
        report(VOICE_STATE_ERROR, "Voice service error");
    }
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *event = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        unsigned attempt = atomic_fetch_add(&reconnect_count, 1);
        char hello[384];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"token\":\"%s\",\"sampleRate\":%d,\"format\":\"pcm_s16le\"}",
                 runtime_config.token, AUDIO_RATE);
        esp_websocket_client_send_text(websocket, hello, strlen(hello), portMAX_DELAY);
        /* Phase 1.2: make recovery visible. The first CONNECTED is the initial
         * session; every later one is a reconnect, and the user needs to know
         * the device came back rather than sitting on "reconnecting" forever. */
        if (attempt > 0) {
            ESP_LOGI(TAG, "Voice service reconnected (session %u)", attempt + 1);
            report(VOICE_STATE_CONNECTING, "Voice service reconnected");
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        flush_speaker_queue();
        ESP_LOGW(TAG, "Voice service disconnected; auto-reconnect in 3 s (session %u)",
                 atomic_load(&reconnect_count) + 1);
        report(VOICE_STATE_CONNECTING, "Voice service reconnecting...");
    } else if (id == WEBSOCKET_EVENT_DATA && event && event->data_len > 0) {
        if (event->op_code == 0x1) handle_text(event->data_ptr, event->data_len);
        else if (event->op_code == 0x2 || event->op_code == 0x0)
            queue_speaker_audio((const uint8_t *)event->data_ptr, event->data_len);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        report(VOICE_STATE_ERROR, "Voice WebSocket error");
    }
}

static void microphone_task(void *arg)
{
    int16_t *buffer = arg;
    int bytes = afe->get_feed_chunksize(afe_data) * sizeof(*buffer);
    for (;;) {
        if (esp_codec_dev_read(microphone, buffer, bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Microphone read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        afe->feed(afe_data, buffer);
        if (recording && socket_ready) {
            xSemaphoreTake(recording_lock, portMAX_DELAY);
            if (recording && socket_ready)
                esp_websocket_client_send_bin(websocket, (const char *)buffer, bytes, portMAX_DELAY);
            xSemaphoreGive(recording_lock);
        }
    }
}

static void speaker_task(void *arg)
{
    (void)arg;
    audio_chunk_t *chunk;
    for (;;) {
        if (xQueueReceive(speaker_queue, &chunk, portMAX_DELAY) == pdTRUE) {
            if (chunk->chime) play_wake_chime();
            else if (esp_codec_dev_write(speaker, chunk->data, (int)chunk->length) != ESP_CODEC_DEV_OK)
                ESP_LOGW(TAG, "Speaker write failed");
            last_speaker_tick = xTaskGetTickCount();
            atomic_fetch_sub(&speaker_pending, 1);
            unsigned left = atomic_fetch_sub(&speaker_bytes, chunk->length) - chunk->length;
            if (left <= SPEAKER_LOW_WATER_BYTES && atomic_exchange(&speaker_high, false))
                ESP_LOGI(TAG, "Speaker watermark low: %u bytes queued", left);
            free(chunk);
        }
    }
}

static esp_err_t speech_init(void)
{
    speech_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(speech_models, ESP_ERR_NOT_FOUND, TAG, "Speech model partition missing");
    char *wakenet_name = esp_srmodel_filter(speech_models, ESP_WN_PREFIX, NULL);
    ESP_RETURN_ON_FALSE(wakenet_name, ESP_ERR_NOT_FOUND, TAG, "WakeNet model missing");

    afe_config_t *config = afe_config_init("M", speech_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AFE config allocation failed");
    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->agc_init = true;
    config->wakenet_init = true;
    config->wakenet_model_name = wakenet_name;
    config->wakenet_model_name_2 = NULL;
    config->vad_init = true;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->fixed_output_channel = true;
    config->output_playback_channel = false;
    config->afe_perferred_core = 1;
    afe = esp_afe_handle_from_config(config);
    if (afe) afe_data = afe->create_from_config(config);
    afe_config_free(config);
    ESP_RETURN_ON_FALSE(afe_data, ESP_ERR_NO_MEM, TAG, "AFE allocation failed");
    ESP_RETURN_ON_FALSE(afe->get_feed_channel_num(afe_data) == 1,
                        ESP_ERR_INVALID_SIZE, TAG, "Incompatible speech audio format");
    ESP_LOGI(TAG, "Wake word \"%s\" (%s) feed=%d fetch=%d",
             WAKE_WORD, wakenet_name, afe->get_feed_chunksize(afe_data), afe->get_fetch_chunksize(afe_data));
    return ESP_OK;
}

/* MultiNet lives only while the voice bridge is unreachable; cloud conversation
 * mode gets core 1 and the internal RAM back. Created on offline wake, destroyed
 * as soon as the bridge reports ready. */
static esp_err_t commands_start(void)
{
    if (command_model) return ESP_OK;
    char *name = esp_srmodel_filter(speech_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    ESP_RETURN_ON_FALSE(name, ESP_ERR_NOT_FOUND, TAG, "Chinese MultiNet model missing");
    multinet = esp_mn_handle_from_name(name);
    ESP_RETURN_ON_FALSE(multinet, ESP_ERR_NOT_SUPPORTED, TAG, "Unsupported MultiNet model");
    /* MultiNet7 creation plus the command FST build is one multi-second
     * CPU-bound library call. At speech_task's normal priority it starves IDLE1
     * for longer than the 5 s task watchdog and trips it (p0-verify-600s.log,
     * 420821 ms, backtrace into dl_nn_pointwise_conv1d). Drop to the idle
     * priority for the duration so round-robin time slicing lets IDLE1 feed the
     * watchdog; the AFE ring buffer absorbs the delayed fetch. */
    UBaseType_t saved_priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 0);
    command_model = multinet->create(name, COMMAND_WINDOW_MS);
    esp_err_t err = ESP_OK;
    if (!command_model) {
        err = ESP_ERR_NO_MEM;
    } else if (afe->get_fetch_chunksize(afe_data) != multinet->get_samp_chunksize(command_model) ||
               multinet->get_samp_rate(command_model) != AUDIO_RATE) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) err = esp_mn_commands_alloc(multinet, command_model);
    if (err == ESP_OK) {
        for (size_t i = 0; i < sizeof(voice_commands) / sizeof(voice_commands[0]); i++) {
            err = esp_mn_commands_add(voice_commands[i].id, voice_commands[i].pinyin);
            if (err != ESP_OK) break;
        }
        if (err == ESP_OK && esp_mn_commands_update() != 0) err = ESP_ERR_INVALID_ARG;
        if (err != ESP_OK) esp_mn_commands_free();
    }
    if (err != ESP_OK && command_model) {
        multinet->destroy(command_model);
        command_model = NULL;
    }
    vTaskPrioritySet(NULL, saved_priority);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Offline commands unavailable: %s", esp_err_to_name(err));
        return err;
    }
    multinet->print_active_speech_commands(command_model);
    ESP_LOGI(TAG, "Offline commands loaded; free internal=%u dma_block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return ESP_OK;
}

static void commands_stop(void)
{
    if (!command_model) return;
    esp_mn_commands_free();
    multinet->destroy(command_model);
    command_model = NULL;
    ESP_LOGI(TAG, "MultiNet released for cloud mode; free internal=%u dma_block=%u PSRAM=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static bool wake_blocked(void)
{
    return recording || remote_turn_active || atomic_load(&speaker_pending) != 0 ||
           (TickType_t)(xTaskGetTickCount() - atomic_load(&last_speaker_tick)) < pdMS_TO_TICKS(WAKE_ECHO_GUARD_MS);
}

static void dispatch_command(int id, const char *phrase, float probability)
{
    for (size_t i = 0; i < sizeof(voice_commands) / sizeof(voice_commands[0]); i++) {
        if (voice_commands[i].id != id) continue;
        ESP_LOGI(TAG, "Command \"%s\" id=%d probability=%.3f", phrase, id, (double)probability);
        if (action_callback) action_callback(voice_commands[i].action);
        return;
    }
    ESP_LOGW(TAG, "Unbound command id=%d phrase=\"%s\"", id, phrase);
}

static void speech_task(void *arg)
{
    (void)arg;
    esp_err_t err = speech_init();
    int16_t *buffer = NULL;
    if (err == ESP_OK) {
        buffer = malloc(afe->get_feed_chunksize(afe_data) * sizeof(*buffer));
        if (!buffer || xTaskCreatePinnedToCore(microphone_task, "voice_mic", 4096, buffer, 6, NULL, 0) != pdPASS)
            err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        free(buffer);
        if (afe_data) afe->destroy(afe_data);
        if (command_model) {
            esp_mn_commands_free();
            multinet->destroy(command_model);
        }
        if (speech_models) esp_srmodel_deinit(speech_models);
        ESP_LOGE(TAG, "Offline speech init failed: %s", esp_err_to_name(err));
        report(VOICE_STATE_ERROR, "Offline speech init failed");
        vTaskDelete(NULL);
        return;
    }
    speech_ready = true;
    ESP_LOGI(TAG, "Listening offline for \"%s\"; free internal=%u dma_block=%u PSRAM=%u", WAKE_WORD,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool listening = false;
    int64_t resume_after = esp_timer_get_time() + WAKE_ECHO_GUARD_MS * 1000LL;
    int64_t command_deadline = 0;
    unsigned wake_count = 0;
    for (;;) {
        afe_fetch_result_t *result = afe->fetch(afe_data);
        if (!result || result->ret_value == ESP_FAIL) {
            if (command_model) multinet->clean(command_model);
            ESP_LOGW(TAG, "Speech audio fetch failed");
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (command_model && socket_ready && !listening) commands_stop();
        if (!listening) {
            /* Only WakeNet may run here; detecting every frame with MultiNet starves the core. */
            if (result->wakeup_state != WAKENET_DETECTED || wake_blocked() || now < resume_after) continue;
            ESP_LOGI(TAG, "Wake detected: \"%s\" count=%u", WAKE_WORD, ++wake_count);
            queue_wake_chime();
            report(VOICE_STATE_WAKE, WAKE_WORD);
            if (socket_ready || commands_start() != ESP_OK) {
                /* Cloud conversation mode: chime + wave only, no command window. */
                resume_after = now + WAKE_COOLDOWN_US;
                continue;
            }
            afe->disable_wakenet(afe_data);
            multinet->clean(command_model);
            listening = true;
            command_deadline = now + COMMAND_WINDOW_MS * 1000LL;
            continue;
        }
        bool closed = now >= command_deadline || socket_ready || !command_model;
        if (!closed && !wake_blocked()) {
            esp_mn_state_t state = multinet->detect(command_model, result->data);
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *matches = multinet->get_results(command_model);
                if (matches && matches->num > 0)
                    dispatch_command(matches->command_id[0], matches->string, matches->prob[0]);
                closed = true;
            } else if (state == ESP_MN_STATE_TIMEOUT) {
                closed = true;
            }
        }
        if (!closed) continue;
        ESP_LOGI(TAG, "Command window closed");
        listening = false;
        if (command_model) multinet->clean(command_model);
        afe->enable_wakenet(afe_data);
        resume_after = esp_timer_get_time() + WAKE_COOLDOWN_US;
    }
}

static void connection_task(void *arg)
{
    (void)arg;
    esp_err_t err = network_stack_init();
    if (err != ESP_OK) {
        report(VOICE_STATE_ERROR, "Network initialization failed");
        vTaskDelete(NULL);
    }
    esp_err_t config_state = load_runtime_config(&runtime_config);
    if (config_state != ESP_OK) {
        if (provisioning_start(&runtime_config) != ESP_OK) report(VOICE_STATE_ERROR, "Wi-Fi setup failed");
        vTaskDelete(NULL);
    }
    report(VOICE_STATE_CONNECTING, "Connecting Wi-Fi...");
    err = wifi_start(&runtime_config);
    if (err != ESP_OK) {
        report(VOICE_STATE_ERROR, "Configure Wi-Fi in menuconfig");
        vTaskDelete(NULL);
    }
    EventBits_t result = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                              pdFALSE, pdFALSE, portMAX_DELAY);
    if (result & WIFI_FAILED_BIT) vTaskDelete(NULL);
    report(VOICE_STATE_CONNECTING, "Connecting voice service...");
    esp_websocket_client_config_t config = {
        .uri = runtime_config.bridge_uri,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 3000,
        .task_stack = 8192,
    };
    if (!strncmp(runtime_config.bridge_uri, "wss://", 6))
        config.crt_bundle_attach = esp_crt_bundle_attach;
    websocket = esp_websocket_client_init(&config);
    if (!websocket) {
        report(VOICE_STATE_ERROR, "Voice client init failed");
        vTaskDelete(NULL);
    }
    esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    if (esp_websocket_client_start(websocket) != ESP_OK)
        report(VOICE_STATE_ERROR, "Voice client start failed");
    vTaskDelete(NULL);
}

esp_err_t voice_client_start(voice_state_callback_t callback, voice_action_callback_t action_cb)
{
    state_callback = callback;
    action_callback = action_cb;
    wifi_events = xEventGroupCreate();
    speaker_queue = xQueueCreate(SPEAKER_QUEUE_SLOTS, sizeof(audio_chunk_t *));
    recording_lock = xSemaphoreCreateMutex();
    if (!wifi_events || !speaker_queue || !recording_lock) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(audio_start(), TAG, "audio init");
    if (xTaskCreate(speaker_task, "voice_speaker", 4096, NULL, 6, NULL) != pdPASS ||
        xTaskCreatePinnedToCore(speech_task, "voice_speech", 16384, NULL, 5, NULL, 1) != pdPASS ||
        xTaskCreate(connection_task, "voice_connect", 6144, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool voice_client_enabled(void) { return true; }
bool voice_client_ready(void) { return socket_ready && speech_ready; }

void voice_client_begin_turn(void)
{
    if (!speech_ready || !socket_ready || recording || remote_turn_active || atomic_load(&speaker_pending)) return;
    xSemaphoreTake(recording_lock, portMAX_DELAY);
    remote_turn_active = true;
    const char *start = "{\"type\":\"start\"}";
    if (esp_websocket_client_send_text(websocket, start, strlen(start), portMAX_DELAY) >= 0) {
        recording = true;
        report(VOICE_STATE_LISTENING, "Listening... release BOOT to send");
    } else {
        remote_turn_active = false;
    }
    xSemaphoreGive(recording_lock);
}

void voice_client_end_turn(void)
{
    if (!recording) return;
    xSemaphoreTake(recording_lock, portMAX_DELAY);
    recording = false;
    const char *stop = "{\"type\":\"stop\"}";
    if (esp_websocket_client_send_text(websocket, stop, strlen(stop), portMAX_DELAY) >= 0) {
        report(VOICE_STATE_THINKING, "Thinking...");
    } else {
        remote_turn_active = false;
        report(VOICE_STATE_ERROR, "Could not send recording");
    }
    xSemaphoreGive(recording_lock);
}

void voice_client_reset_config(void)
{
    if (resetting_config) return;
    resetting_config = true;
    ESP_LOGW(TAG, "Config reset requested; erasing NVS and restarting into AP mode");
    report(VOICE_STATE_CONNECTING, "Resetting setup...");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        nvs_handle_t handle;
        err = nvs_open("qiban_voice", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_erase_all(handle);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
    }
    if (err != ESP_OK) {
        resetting_config = false;
        report(VOICE_STATE_ERROR, "Could not reset setup");
        return;
    }
    xTaskCreate(restart_task, "setup_reset", 2048, NULL, 5, NULL);
}

esp_err_t voice_client_wifi_scan(void)
{
    if (!wifi_driver_ready) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&wifi_data_lock);
    if (scan_in_progress) {
        portEXIT_CRITICAL(&wifi_data_lock);
        return ESP_OK;
    }
    scan_in_progress = true;
    portEXIT_CRITICAL(&wifi_data_lock);
    wifi_scan_config_t scan = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&wifi_data_lock);
        scan_in_progress = false;
        portEXIT_CRITICAL(&wifi_data_lock);
        return err;
    }
    report(VOICE_STATE_WIFI_SCANNING, "Scanning Wi-Fi...");
    return ESP_OK;
}

esp_err_t voice_client_wifi_connect(const char *ssid, const char *password)
{
    if (!wifi_driver_ready || !ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64))
        return ESP_ERR_INVALID_ARG;
    strlcpy(runtime_config.ssid, ssid, sizeof(runtime_config.ssid));
    strlcpy(runtime_config.password, password ? password : "", sizeof(runtime_config.password));
    ESP_RETURN_ON_ERROR(save_wifi_config(&runtime_config), TAG, "save selected Wi-Fi");
    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, runtime_config.ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, runtime_config.password, sizeof(station.sta.password));
    station.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_retries = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), TAG, "set selected Wi-Fi");
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) report(VOICE_STATE_CONNECTING, "Connecting selected Wi-Fi...");
    return err;
}

size_t voice_client_wifi_networks(voice_wifi_network_t *networks, size_t capacity)
{
    if (!networks || !capacity) return 0;
    portENTER_CRITICAL(&wifi_data_lock);
    size_t count = wifi_network_count < capacity ? wifi_network_count : capacity;
    memcpy(networks, wifi_networks, count * sizeof(*networks));
    portEXIT_CRITICAL(&wifi_data_lock);
    return count;
}

bool voice_client_wifi_info(char *ssid, size_t ssid_capacity, char *ip, size_t ip_capacity)
{
    portENTER_CRITICAL(&wifi_data_lock);
    bool connected = wifi_connected;
    if (ssid && ssid_capacity) strlcpy(ssid, connected_ssid, ssid_capacity);
    if (ip && ip_capacity) strlcpy(ip, connected_ip, ip_capacity);
    portEXIT_CRITICAL(&wifi_data_lock);
    return connected;
}

#else

esp_err_t voice_client_start(voice_state_callback_t callback, voice_action_callback_t action_cb)
{
    (void)action_cb;
    if (callback) callback(VOICE_STATE_DISABLED, NULL);
    return ESP_ERR_NOT_SUPPORTED;
}
bool voice_client_enabled(void) { return false; }
bool voice_client_ready(void) { return false; }
void voice_client_begin_turn(void) {}
void voice_client_end_turn(void) {}
void voice_client_reset_config(void) {}
esp_err_t voice_client_wifi_scan(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t voice_client_wifi_connect(const char *ssid, const char *password)
{ (void)ssid; (void)password; return ESP_ERR_NOT_SUPPORTED; }
size_t voice_client_wifi_networks(voice_wifi_network_t *networks, size_t capacity)
{ (void)networks; (void)capacity; return 0; }
bool voice_client_wifi_info(char *ssid, size_t ssid_capacity, char *ip, size_t ip_capacity)
{ (void)ssid; (void)ssid_capacity; (void)ip; (void)ip_capacity; return false; }

#endif
