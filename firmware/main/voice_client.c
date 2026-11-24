#include "sdkconfig.h"
#include "voice_client.h"
#include "voice_defaults.h"

#if CONFIG_WHALE_VOICE_ENABLED

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdatomic.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
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
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"

#define AUDIO_RATE 16000
#define MIC_CHUNK_SAMPLES 512
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAX_RETRIES 8

typedef struct {
    size_t length;
    uint8_t data[];
} audio_chunk_t;

typedef struct {
    char ssid[33];
    char password[65];
    char bridge_uri[256];
    char token[129];
} runtime_config_t;

static const char *TAG = "voice_client";
static EventGroupHandle_t wifi_events;
static QueueHandle_t speaker_queue;
static SemaphoreHandle_t recording_lock;
static esp_websocket_client_handle_t websocket;
static esp_codec_dev_handle_t microphone;
static esp_codec_dev_handle_t speaker;
static voice_state_callback_t state_callback;
static voice_action_callback_t action_callback;
static voice_overlay_callback_t overlay_callback;
static atomic_bool recording;
static atomic_bool socket_ready;
static atomic_bool remote_turn_active;
/* True once any reply audio has been queued this turn. The speaker task uses it
 * to tell "still thinking, no audio yet" apart from "reply finished playing",
 * so the local standby reset can never fire mid-thought. */
static atomic_bool turn_audio_started;
static atomic_bool mic_ready;
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
static int wifi_retries;
static runtime_config_t runtime_config;
static bool resetting_config;
static bool wifi_driver_ready;

static const char provisioning_page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>栖伴配网</title><style>body{font:16px system-ui;max-width:520px;margin:35px auto;padding:18px}"
    "input{box-sizing:border-box;width:100%%;padding:12px;margin:6px 0 16px;border:1px solid #aaa;border-radius:8px}"
    "button{padding:12px 20px;border:0;border-radius:8px;background:#2b7655;color:white;font-weight:700}"
    ".scan{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 16px}"
    ".scan button{background:#eef3f0;color:#2b7655;padding:8px 12px;font-weight:400}"
    "details{margin:0 0 16px;color:#555}summary{cursor:pointer;padding:8px 0}</style>"
    "<h1>栖伴 · 极简配网</h1><p>选择 2.4 GHz Wi-Fi 并输入密码，语音桥地址和令牌已预填。</p>"
    "<form method=post action=/save><label>Wi-Fi 名称</label><input name=ssid id=ssid maxlength=32 required>"
    "<div class=scan id=scan>正在扫描…</div>"
    "<label>Wi-Fi 密码</label><input name=password type=password maxlength=64>"
    "<details><summary>高级设置（已预填，一般无需修改）</summary>"
    "<label>语音桥地址</label><input name=bridge maxlength=255 required value='%s'>"
    "<label>设备令牌</label><input name=token maxlength=128 required value='%s'>"
    "</details><button>保存并重启</button></form>"
    "<script>function load(){fetch('/scan').then(function(r){return r.json();}).then(function(list){"
    "var box=document.getElementById('scan');box.innerHTML='';"
    "if(!list.length){box.textContent='未扫描到 Wi-Fi，请手动输入名称';return;}"
    "list.forEach(function(ap){var b=document.createElement('button');b.type='button';"
    "b.textContent=ap.ssid+' ('+ap.rssi+' dBm)';"
    "b.onclick=function(){document.getElementById('ssid').value=ap.ssid;};box.appendChild(b);});"
    "var again=document.createElement('button');again.type='button';again.textContent='重新扫描';"
    "again.onclick=load;box.appendChild(again);}).catch(function(){"
    "document.getElementById('scan').textContent='扫描失败，请手动输入名称';});}load();</script>";

static void report(voice_state_t state, const char *message)
{
    if (state_callback) state_callback(state, message);
}

static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "认证超时";
    case WIFI_REASON_AUTH_LEAVE: return "认证离开";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "因不活跃被解除关联";
    case WIFI_REASON_ASSOC_TOOMANY: return "AP 客户端已满";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "未关联";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "已关联但未认证";
    case WIFI_REASON_ASSOC_LEAVE: return "关联离开";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "四次握手超时（多为密码错误）";
    case WIFI_REASON_BEACON_TIMEOUT: return "信标超时（信号弱或超出范围）";
    case WIFI_REASON_NO_AP_FOUND: return "找不到 AP（SSID 不存在或为 5GHz）";
    case WIFI_REASON_AUTH_FAIL: return "认证失败（多为密码错误）";
    case WIFI_REASON_ASSOC_FAIL: return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "握手超时（多为密码错误）";
    default: return "其他原因，见 wifi_err_reason_t";
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (runtime_config.ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "STA disconnected: reason=%d (%s), retry %d/%d",
                 event->reason, wifi_reason_name(event->reason), wifi_retries + 1, WIFI_MAX_RETRIES);
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        turn_audio_started = false;
        if (wifi_retries++ < WIFI_MAX_RETRIES) {
            report(VOICE_STATE_WIFI_DISCONNECTED, "网络断开，重连中…");
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
            report(VOICE_STATE_WIFI_DISCONNECTED, "网络连接失败");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_retries = 0;
        esp_wifi_set_ps(WIFI_PS_NONE);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        report(VOICE_STATE_WIFI_CONNECTED, "网络已连接");
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
        size_t token = sizeof(config->token);
        esp_err_t have_ssid = nvs_get_str(handle, "ssid", config->ssid, &ssid);
        nvs_get_str(handle, "password", config->password, &password);
        esp_err_t have_token = nvs_get_str(handle, "token", config->token, &token);
        nvs_close(handle);
        if (have_ssid == ESP_OK && config->ssid[0]) {
            /* Bridge URL is permanently baked into the firmware: changing it
             * needs only a reflash, never re-provisioning. Wi-Fi stays in NVS. */
            strlcpy(config->bridge_uri, DEFAULT_BRIDGE_URL, sizeof(config->bridge_uri));
            if (have_token != ESP_OK || !config->token[0])
                strlcpy(config->token, DEFAULT_DEVICE_TOKEN, sizeof(config->token));
            return ESP_OK;
        }
    }
    if (CONFIG_WHALE_WIFI_SSID[0]) {
        strlcpy(config->ssid, CONFIG_WHALE_WIFI_SSID, sizeof(config->ssid));
        strlcpy(config->password, CONFIG_WHALE_WIFI_PASSWORD, sizeof(config->password));
        strlcpy(config->bridge_uri, DEFAULT_BRIDGE_URL, sizeof(config->bridge_uri));
        strlcpy(config->token, DEFAULT_DEVICE_TOKEN, sizeof(config->token));
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

static esp_err_t setup_get(httpd_req_t *request)
{
    size_t capacity = sizeof(provisioning_page) + sizeof(DEFAULT_BRIDGE_URL) + sizeof(DEFAULT_DEVICE_TOKEN) + 64;
    char *page = malloc(capacity);
    if (!page) return ESP_ERR_NO_MEM;
    snprintf(page, capacity, provisioning_page, DEFAULT_BRIDGE_URL, DEFAULT_DEVICE_TOKEN);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t setup_scan(httpd_req_t *request)
{
    wifi_scan_config_t scan = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    uint16_t count = 0;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK || esp_wifi_scan_get_ap_num(&count) != ESP_OK) {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_sendstr(request, "[]");
        return ESP_OK;
    }
    if (count > 20) count = 20;
    wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    char *json = malloc(20 * 100 + 8);
    if (!records || !json) { free(records); free(json); return ESP_ERR_NO_MEM; }
    esp_wifi_scan_get_ap_records(&count, records);
    size_t used = 0;
    uint16_t listed = 0;
    json[used++] = '[';
    for (uint16_t i = 0; i < count; i++) {
        if (!records[i].ssid[0]) continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i; j++)
            if (!strcmp((char *)records[j].ssid, (char *)records[i].ssid)) { duplicate = true; break; }
        if (duplicate) continue;
        char entry[100];
        int n = snprintf(entry, sizeof(entry), "%s{\"ssid\":\"", listed ? "," : "");
        for (const uint8_t *c = records[i].ssid; *c && n < (int)sizeof(entry) - 24; c++) {
            if (*c == '"' || *c == '\\') n += snprintf(entry + n, sizeof(entry) - n, "\\%c", *c);
            else if (*c >= 0x20) entry[n++] = (char)*c;
        }
        n += snprintf(entry + n, sizeof(entry) - n, "\",\"rssi\":%d}", records[i].rssi);
        if (used + (size_t)n + 2 < 20 * 100 + 8) { memcpy(json + used, entry, n); used += n; listed++; }
    }
    json[used++] = ']';
    json[used] = '\0';
    free(records);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, json);
    free(json);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t setup_save(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 1200) {
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

static esp_err_t provisioning_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_name[33];
    snprintf(ap_name, sizeof(ap_name), "Qiban-%02X%02X", mac[4], mac[5]);
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "AP Wi-Fi init");
    wifi_config_t config = {0};
    strlcpy((char *)config.ap.ssid, ap_name, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, "qiban1234", sizeof(config.ap.password));
    config.ap.ssid_len = strlen(ap_name);
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* APSTA lets the phone pick a scanned 2.4 GHz SSID from the page. The STA
     * side only scans during provisioning; it connects after /save reboots. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");
    wifi_driver_ready = true;
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &http_config), TAG, "setup web server");
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = setup_get };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = setup_scan };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = setup_save };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &save);
    char status[96];
    snprintf(status, sizeof(status), "手机配网：连接 %s\n打开 192.168.4.1", ap_name);
    report(VOICE_STATE_CONNECTING, status);
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
    memcpy(chunk->data, data, length);
    atomic_store(&turn_audio_started, true);
    enqueue_speaker_chunk(chunk);
}

static void handle_text(const char *data, size_t length)
{
    /* The bridge protocol has a deliberately tiny fixed state vocabulary. Avoid
     * pulling a JSON DOM into the firmware for messages we do not need to retain. */
    char message[384];
    size_t used = length < sizeof(message) - 1 ? length : sizeof(message) - 1;
    memcpy(message, data, used);
    message[used] = '\0';
    if (strstr(message, "\"type\":\"display_overlay\"")) {
        cJSON *root = cJSON_Parse(message);
        if (root) {
            cJSON *url = cJSON_GetObjectItem(root, "url");
            cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
            if (cJSON_IsString(url) && overlay_callback) {
                unsigned ms = cJSON_IsNumber(dur) ? (unsigned)dur->valuedouble : 12000;
                overlay_callback(url->valuestring, ms);
            }
            cJSON_Delete(root);
        }
        return;
    }
    if (!strstr(message, "\"type\":\"state\"")) return;
    if (strstr(message, "\"state\":\"ready\"") || strstr(message, "\"state\":\"idle\"")) {
        socket_ready = true;
        recording = false;
        remote_turn_active = false;
        turn_audio_started = false;
        report(VOICE_STATE_READY, "按住按键说话");
    } else if (strstr(message, "\"state\":\"listening\"")) {
        report(VOICE_STATE_LISTENING, "聆听中，松开发送");
    } else if (strstr(message, "\"state\":\"thinking\"")) {
        report(VOICE_STATE_THINKING, "思考中…");
    } else if (strstr(message, "\"state\":\"speaking\"")) {
        report(VOICE_STATE_SPEAKING, "说话中…");
    } else if (strstr(message, "\"state\":\"error\"")) {
        recording = false;
        remote_turn_active = false;
        turn_audio_started = false;
        report(VOICE_STATE_ERROR, "语音服务异常");
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
            report(VOICE_STATE_CONNECTING, "语音服务已重连");
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED) {
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        turn_audio_started = false;
        flush_speaker_queue();
        ESP_LOGW(TAG, "Voice service disconnected; auto-reconnect in 3 s (session %u)",
                 atomic_load(&reconnect_count) + 1);
        report(VOICE_STATE_CONNECTING, "语音服务重连中…");
    } else if (id == WEBSOCKET_EVENT_DATA && event && event->data_len > 0) {
        if (event->op_code == 0x1) handle_text(event->data_ptr, event->data_len);
        else if (event->op_code == 0x2 || event->op_code == 0x0)
            queue_speaker_audio((const uint8_t *)event->data_ptr, event->data_len);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        socket_ready = false;
        recording = false;
        remote_turn_active = false;
        turn_audio_started = false;
        report(VOICE_STATE_ERROR, "语音连接异常");
    }
}

/* Push-to-talk capture: raw 16 kHz mono PCM straight from the codec, no AFE,
 * no WakeNet, no MultiNet. The cloud does all recognition. */
static void microphone_task(void *arg)
{
    int16_t *buffer = arg;
    const int bytes = MIC_CHUNK_SAMPLES * sizeof(*buffer);
    for (;;) {
        if (esp_codec_dev_read(microphone, buffer, bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Microphone read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (recording && socket_ready) {
            xSemaphoreTake(recording_lock, portMAX_DELAY);
            if (recording && socket_ready) {
                /* Bounded: this lock is shared with voice_client_begin_turn(),
                 * which runs in the button task. An unbounded WS send here
                 * could freeze BOOT press/release detection on a network stall. */
                esp_websocket_client_send_bin(websocket, (const char *)buffer, bytes, pdMS_TO_TICKS(100));
            }
            xSemaphoreGive(recording_lock);
        }
    }
}

static void speaker_task(void *arg)
{
    (void)arg;
    audio_chunk_t *chunk;
    int64_t quiet_since = 0;
    for (;;) {
        /* While a turn is open, wake up periodically even with an empty queue:
         * the bridge's end-of-turn "idle" message is the normal reset path, but
         * if it never arrives the board must still return to standby on its own
         * once the reply has finished playing. */
        TickType_t wait = atomic_load(&remote_turn_active) ? pdMS_TO_TICKS(250) : portMAX_DELAY;
        if (xQueueReceive(speaker_queue, &chunk, wait) == pdTRUE && chunk) {
            quiet_since = 0;
            if (esp_codec_dev_write(speaker, chunk->data, (int)chunk->length) != ESP_CODEC_DEV_OK)
                ESP_LOGW(TAG, "Speaker write failed");
            atomic_fetch_sub(&speaker_pending, 1);
            unsigned left = atomic_fetch_sub(&speaker_bytes, chunk->length) - chunk->length;
            if (left <= SPEAKER_LOW_WATER_BYTES && atomic_exchange(&speaker_high, false))
                ESP_LOGI(TAG, "Speaker watermark low: %u bytes queued", left);
            free(chunk);
        } else if (atomic_load(&remote_turn_active) && !atomic_load(&recording) &&
                   atomic_load(&turn_audio_started) && atomic_load(&speaker_pending) == 0) {
            int64_t now = esp_timer_get_time();
            if (!quiet_since) quiet_since = now;
            else if (now - quiet_since >= 3000000) {
                quiet_since = 0;
                atomic_store(&remote_turn_active, false);
                atomic_store(&turn_audio_started, false);
                ESP_LOGW(TAG, "No end-of-turn ack from bridge; local reset to standby");
                report(VOICE_STATE_READY, "按住按键说话");
            }
        } else {
            quiet_since = 0;
        }
    }
}

static void sync_time_for_tls(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.num_of_servers = 3;
    cfg.servers[1] = "ntp.tencent.com";
    cfg.servers[2] = "time.google.com";
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed; TLS certificate time check may fail");
        return;
    }
    esp_err_t r = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (r == ESP_OK) {
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        ESP_LOGI(TAG, "Time synced via SNTP: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                 tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout (%s); TLS certificate validation may fail", esp_err_to_name(r));
    }
}

static void connection_task(void *arg)
{
    (void)arg;
    esp_err_t err = network_stack_init();
    if (err != ESP_OK) {
        report(VOICE_STATE_ERROR, "网络初始化失败");
        vTaskDelete(NULL);
    }
    esp_err_t config_state = load_runtime_config(&runtime_config);
    if (config_state != ESP_OK) {
        if (provisioning_start() != ESP_OK) report(VOICE_STATE_ERROR, "配网启动失败");
        vTaskDelete(NULL);
    }
    report(VOICE_STATE_CONNECTING, "正在连接网络…");
    err = wifi_start(&runtime_config);
    if (err != ESP_OK) {
        report(VOICE_STATE_ERROR, "请在 menuconfig 配置 Wi-Fi");
        vTaskDelete(NULL);
    }
    EventBits_t result = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                              pdFALSE, pdFALSE, portMAX_DELAY);
    if (result & WIFI_FAILED_BIT) {
        /* Never give up silently: a board that stops retrying looks exactly
         * like a dead BOOT key once the AP returns. Slow-retry every 30 s. */
        ESP_LOGW(TAG, "Wi-Fi fast retries exhausted; slow retry every 30 s");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            xEventGroupClearBits(wifi_events, WIFI_FAILED_BIT | WIFI_CONNECTED_BIT);
            esp_wifi_connect();
            result = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);
            if (result & WIFI_CONNECTED_BIT) break;
            ESP_LOGW(TAG, "Wi-Fi slow retry failed; next in 30 s");
        }
    }
    if (!strncmp(runtime_config.bridge_uri, "wss://", 6))
        sync_time_for_tls();
    report(VOICE_STATE_CONNECTING, "正在连接语音服务…");
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
        report(VOICE_STATE_ERROR, "语音客户端初始化失败");
        vTaskDelete(NULL);
    }
    esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    if (esp_websocket_client_start(websocket) != ESP_OK)
        report(VOICE_STATE_ERROR, "语音客户端启动失败");
    vTaskDelete(NULL);
}

void voice_client_set_overlay(voice_overlay_callback_t callback)
{
    overlay_callback = callback;
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
    int16_t *mic_buffer = heap_caps_malloc(MIC_CHUNK_SAMPLES * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mic_buffer) return ESP_ERR_NO_MEM;
    if (xTaskCreate(speaker_task, "voice_speaker", 4096, NULL, 6, NULL) != pdPASS ||
        xTaskCreatePinnedToCore(microphone_task, "voice_mic", 4096, mic_buffer, 6, NULL, 1) != pdPASS ||
        xTaskCreate(connection_task, "voice_connect", 6144, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    mic_ready = true;
    ESP_LOGI(TAG, "Push-to-talk capture ready; free internal=%u dma_block=%u PSRAM=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

bool voice_client_enabled(void) { return true; }
bool voice_client_ready(void) { return socket_ready && mic_ready; }
bool voice_client_wifi_connected(void) { return wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT); }
const char *voice_client_token(void) { return runtime_config.token[0] ? runtime_config.token : DEFAULT_DEVICE_TOKEN; }

void voice_client_begin_turn(void)
{
    /* Every rejection is logged with the exact gate that blocked it: a silent
     * return here is what made a stuck turn look like a dead BOOT key. */
    if (!mic_ready) { ESP_LOGW(TAG, "BOOT turn rejected: microphone not ready"); return; }
    if (!socket_ready) { ESP_LOGW(TAG, "BOOT turn rejected: voice bridge not connected"); return; }
    if (recording) { ESP_LOGW(TAG, "BOOT turn rejected: already recording"); return; }
    if (remote_turn_active) {
        ESP_LOGW(TAG, "BOOT turn rejected: previous turn still active (speaker_pending=%u)",
                 atomic_load(&speaker_pending));
        return;
    }
    if (atomic_load(&speaker_pending)) {
        ESP_LOGW(TAG, "BOOT turn rejected: reply still playing (%u chunks queued)",
                 atomic_load(&speaker_pending));
        return;
    }
    if (xSemaphoreTake(recording_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "BOOT turn rejected: recording lock busy");
        return;
    }
    remote_turn_active = true;
    atomic_store(&turn_audio_started, false);
    const char *start = "{\"type\":\"start\"}";
    if (esp_websocket_client_send_text(websocket, start, strlen(start), pdMS_TO_TICKS(2000)) >= 0) {
        recording = true;
        ESP_LOGI(TAG, "BOOT turn started");
        report(VOICE_STATE_LISTENING, "聆听中，松开按键发送");
    } else {
        remote_turn_active = false;
        ESP_LOGW(TAG, "BOOT turn start send failed");
    }
    xSemaphoreGive(recording_lock);
}

void voice_client_end_turn(void)
{
    if (!recording) return;
    if (xSemaphoreTake(recording_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        recording = false;
        remote_turn_active = false;
        ESP_LOGW(TAG, "BOOT turn end: recording lock busy; turn aborted");
        report(VOICE_STATE_ERROR, "录音发送失败");
        return;
    }
    recording = false;
    const char *end = "{\"type\":\"audio_end\"}";
    if (esp_websocket_client_send_text(websocket, end, strlen(end), pdMS_TO_TICKS(2000)) >= 0) {
        ESP_LOGI(TAG, "BOOT turn ended; recording sent");
        report(VOICE_STATE_THINKING, "思考中…");
    } else {
        remote_turn_active = false;
        ESP_LOGW(TAG, "BOOT turn end send failed");
        report(VOICE_STATE_ERROR, "录音发送失败");
    }
    xSemaphoreGive(recording_lock);
}

void voice_client_reset_config(void)
{
    if (resetting_config) return;
    resetting_config = true;
    ESP_LOGW(TAG, "Config reset requested; erasing NVS and restarting into AP mode");
    report(VOICE_STATE_CONNECTING, "正在重置配网…");
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
        report(VOICE_STATE_ERROR, "重置配网失败");
        return;
    }
    xTaskCreate(restart_task, "setup_reset", 2048, NULL, 5, NULL);
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
bool voice_client_wifi_connected(void) { return false; }
const char *voice_client_token(void) { return DEFAULT_DEVICE_TOKEN; }
void voice_client_begin_turn(void) {}
void voice_client_end_turn(void) {}
void voice_client_reset_config(void) {}
void voice_client_set_overlay(voice_overlay_callback_t callback) { (void)callback; }

#endif
