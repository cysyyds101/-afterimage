/* Network character sync: poll the platform for the assigned character, fetch
 * all eight display frames as ONE binary package over a single HTTP connection
 * into PSRAM, swap them into the resident buffers immediately, then persist to
 * the `model` SPIFFS partition in a background task. Local /storage playback
 * remains the boot fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "cJSON.h"
#include "animation_manifest.h"
#include "voice_client.h"
#include "voice_defaults.h"
#include "net_sync.h"

#define TAG "net_sync"
#define MODEL_DIR "/model"
#define MANIFEST_CAP (64 * 1024)
#define BUNDLE_CAP (512 * 1024)

static net_sync_apply_fn apply_fn;
static net_sync_status_fn status_fn;
static char current_version[128];

void net_sync_set_apply(net_sync_apply_fn fn) { apply_fn = fn; }
void net_sync_set_status(net_sync_status_fn fn) { status_fn = fn; }

void net_sync_force(void)
{
    current_version[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("qiban_sync", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "ver");
        nvs_commit(handle);
        nvs_close(handle);
    }
    remove(MODEL_DIR "/version.txt");
    ESP_LOGI(TAG, "Sync version cleared; re-download on next poll");
}

typedef struct {
    char *buf; size_t len, cap;
    bool overflow;
} http_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->buf && evt->data_len > 0) {
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
        } else ctx->overflow = true;
    }
    return ESP_OK;
}

static esp_http_client_handle_t http_open(const char *url, const char *method, http_ctx_t *ctx)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method[0] == 'P' ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = ctx,
        .buffer_size = 32768,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", voice_client_token());
    esp_http_client_set_header(client, "Authorization", auth);
    return client;
}

static bool fetch_manifest(char *out, size_t cap, const char *version)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/api/v1/device/manifest?version=%s", DEFAULT_PLATFORM_URL, version);
    http_ctx_t ctx = { .buf = out, .cap = cap };
    esp_http_client_handle_t client = http_open(url, "GET", &ctx);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || ctx.overflow) {
        ESP_LOGW(TAG, "Manifest fetch failed: err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }
    out[ctx.len] = '\0';
    return true;
}

/* Single connection, whole package into PSRAM. No flash writes on this path. */
static bool download_bundle(uint8_t **out, size_t *out_len)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/api/v1/device/bundle", DEFAULT_PLATFORM_URL);
    char *buf = malloc(BUNDLE_CAP);
    if (!buf) { ESP_LOGE(TAG, "Bundle buffer alloc failed"); return false; }
    http_ctx_t ctx = { .buf = buf, .cap = BUNDLE_CAP };
    esp_http_client_handle_t client = http_open(url, "GET", &ctx);
    if (!client) { free(buf); return false; }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    int64_t t1 = esp_timer_get_time();
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || ctx.overflow || ctx.len < 8) {
        ESP_LOGW(TAG, "Bundle download failed: err=%s status=%d bytes=%u",
                 esp_err_to_name(err), status, (unsigned)ctx.len);
        free(buf);
        return false;
    }
    ESP_LOGI(TAG, "Bundle downloaded: %u bytes in %lld ms (%.1f KB/s)",
             (unsigned)ctx.len, (long long)(t1 - t0) / 1000,
             (double)ctx.len / 1024.0 / (((double)(t1 - t0) / 1e6) + 1e-6));
    *out = (uint8_t *)buf;
    *out_len = ctx.len;
    return true;
}

static void report_result(const char *version, const char *status, const char *error)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/api/v1/device/report", DEFAULT_PLATFORM_URL);
    char body[320];
    if (error) snprintf(body, sizeof(body), "{\"version\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}", version, status, error);
    else snprintf(body, sizeof(body), "{\"version\":\"%s\",\"status\":\"%s\"}", version, status);
    http_ctx_t ctx = {0};
    esp_http_client_handle_t client = http_open(url, "POST", &ctx);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Report %s: err=%s status=%d", status, esp_err_to_name(err), code);
}

static void load_current_version(void)
{
    nvs_handle_t handle;
    if (nvs_open("qiban_sync", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(current_version);
        if (nvs_get_str(handle, "ver", current_version, &len) != ESP_OK) current_version[0] = '\0';
        nvs_close(handle);
    }
    if (!current_version[0]) {
        FILE *fp = fopen(MODEL_DIR "/version.txt", "r");
        if (fp) {
            if (fgets(current_version, sizeof(current_version), fp)) {
                current_version[strcspn(current_version, "\r\n")] = '\0';
            }
            fclose(fp);
        }
    }
}

static void save_current_version(const char *version)
{
    strlcpy(current_version, version, sizeof(current_version));
    nvs_handle_t handle;
    if (nvs_open("qiban_sync", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ver", version);
        nvs_commit(handle);
        nvs_close(handle);
    }
    FILE *fp = fopen(MODEL_DIR "/version.txt", "w");
    if (fp) { fputs(version, fp); fclose(fp); }
}

/* Background flash persistence: runs only AFTER the display swap so playback is
 * never blocked by flash writes. Frees the PSRAM package when done. */
typedef struct { uint8_t *bundle; size_t len; } persist_arg_t;

static void persist_task(void *arg)
{
    persist_arg_t *p = (persist_arg_t *)arg;
    size_t off = 8;
    unsigned written = 0;
    for (unsigned i = 0; i < FRAME_COUNT && off + 4 <= p->len; i++) {
        uint32_t fl;
        memcpy(&fl, p->bundle + off, 4);
        off += 4;
        if (fl == 0 || off + fl > p->len) break;
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", MODEL_DIR, frame_files[i]);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            if (fwrite(p->bundle + off, 1, fl, fp) == fl) written++;
            fclose(fp);
        }
        off += fl;
        /* SPIFFS erase/program disables the flash cache, briefly stalling
         * LVGL's flash-resident refresh. Space frames out so the hitches are
         * spread across the persist window rather than back-to-back. */
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_LOGI(TAG, "Persisted %u/%u frames to flash", written, FRAME_COUNT);
    free(p->bundle);
    free(p);
    vTaskDelete(NULL);
}

static void persist_bundle(uint8_t *bundle, size_t len)
{
    persist_arg_t *p = malloc(sizeof(*p));
    if (!p) { free(bundle); return; }
    p->bundle = bundle;
    p->len = len;
    if (xTaskCreatePinnedToCore(persist_task, "net_persist", 4096, p, 1, NULL, 1) != pdPASS) {
        free(bundle);
        free(p);
    }
}

static void sync_task(void *arg)
{
    (void)arg;
    load_current_version();
    ESP_LOGI(TAG, "Sync ready; platform=%s current=%s", DEFAULT_PLATFORM_URL, current_version[0] ? current_version : "(none)");
    char *manifest = malloc(MANIFEST_CAP);
    if (!manifest) { ESP_LOGE(TAG, "Manifest buffer alloc failed"); vTaskDelete(NULL); return; }
    for (;;) {
        if (!voice_client_wifi_connected()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        if (!fetch_manifest(manifest, MANIFEST_CAP, current_version)) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
        cJSON *root = cJSON_Parse(manifest);
        if (!root) { ESP_LOGW(TAG, "Manifest parse failed"); vTaskDelay(pdMS_TO_TICKS(15000)); continue; }
        cJSON *update = cJSON_GetObjectItem(root, "updateAvailable");
        cJSON *ver = cJSON_GetObjectItem(root, "version");
        cJSON *poll = cJSON_GetObjectItem(root, "pollAfterSeconds");
        int poll_sec = cJSON_IsNumber(poll) ? (int)poll->valuedouble : 30;
        if (poll_sec < 5) poll_sec = 5;
        if (!cJSON_IsTrue(update) || !cJSON_IsString(ver)) {
            cJSON_Delete(root);
            vTaskDelay(pdMS_TO_TICKS(poll_sec * 1000));
            continue;
        }
        char version[128];
        strlcpy(version, ver->valuestring, sizeof(version));
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Update available: %s", version);

        uint8_t *bundle = NULL;
        size_t bundle_len = 0;
        int64_t t0 = esp_timer_get_time();
        if (status_fn) status_fn(true);
        if (!download_bundle(&bundle, &bundle_len)) {
            if (status_fn) status_fn(false);
            report_result(version, "failed", "bundle download failed");
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        int64_t t1 = esp_timer_get_time();
        if (apply_fn && apply_fn(bundle, bundle_len)) {
            int64_t t2 = esp_timer_get_time();
            ESP_LOGI(TAG, "Character on screen: decode+swap %lld ms, download+apply total %lld ms",
                     (long long)(t2 - t1) / 1000, (long long)(t2 - t0) / 1000);
            save_current_version(version);
            report_result(version, "applied", NULL);
            persist_bundle(bundle, bundle_len);
        } else {
            report_result(version, "failed", "decode failed");
            ESP_LOGW(TAG, "Apply failed for %s", version);
            free(bundle);
        }
        if (status_fn) status_fn(false);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t net_sync_start(void)
{
    esp_vfs_spiffs_conf_t fs = {
        .base_path = MODEL_DIR, .partition_label = "model", .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Model SPIFFS: %s", esp_err_to_name(err)); return err; }
    if (xTaskCreatePinnedToCore(sync_task, "net_sync", 8192, NULL, 3, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}