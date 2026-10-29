#include "graph_fetch.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "graph.h"
#include "mqtt.h"
#include "nvs.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "wifi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#    include "esp_crt_bundle.h"
#endif

#define FETCH_RETRY_COUNT 3
#define FETCH_RETRY_DELAY_MS 2000 // 07:31 - jeda 2 s antar percobaan
#define FETCH_WIFI_WAIT_MS 30000  // batas tunggu IP sebelum boot fetch dinyatakan gagal
#define FETCH_SUCCESS_BIT BIT0
#define FETCH_ERROR_BIT BIT1

static const char* TAG = "graph_fetch";

static int s_version;

static EventGroupHandle_t s_fetch_event_group;

bool graph_fetch_success(void) {
    // The group is created in graph_fetch_init() before any caller can run;
    // a NULL handle here means the boot fetch was never started.
    if (s_fetch_event_group == NULL) {
        return false;
    }
    EventBits_t bits =
        xEventGroupWaitBits(s_fetch_event_group, FETCH_SUCCESS_BIT | FETCH_ERROR_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & FETCH_SUCCESS_BIT) != 0;
}

static esp_err_t fetch_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "X-Graph-Version") == 0) {
            s_version = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

static esp_err_t fetch_and_apply(void) {
    nvs_handle_t nvs_handle;
    // Opening can fail (namespace not provisioned yet), so it must not jump to
    // the close label with an invalid handle.
    esp_err_t ret = nvs_open("server", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to open nvs namespace server: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t required_size;
    char server_url[256];
    ESP_GOTO_ON_ERROR(nvs_get_str(nvs_handle, "server.url", NULL, &required_size), nvs_close_on_error, TAG,
                      "Failed to get nvs required size for %s", "server.url");
    ESP_GOTO_ON_ERROR(nvs_get_str(nvs_handle, "server.url", server_url, &required_size), nvs_close_on_error, TAG,
                      "Failed to get %s from nvs", "server.url");

    nvs_close(nvs_handle);

    const char* path = "/graph.bin";

    // Join URL + path in place. A trailing '/' would give "//graph.bin".
    size_t url_len = strlen(server_url);
    if (url_len > 0 && server_url[url_len - 1] == '/') {
        url_len--;
    }
    if (url_len + strlen(path) >= sizeof(server_url)) {
        ESP_LOGE(TAG, "server.url too long (%u bytes)", (unsigned)url_len);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(server_url + url_len, path, strlen(path) + 1);

    esp_http_client_config_t config = {
        .url = server_url,
        .timeout_ms = 5000,
        .event_handler = fetch_event_handler,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    s_version = -1;

    // ESP_RETURN_ON_ERROR would skip esp_http_client_cleanup and leak the client
    err = esp_http_client_set_method(client, HTTP_METHOD_GET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set method GET failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "fetch: open %s", server_url);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        // NB: err still holds ESP_OK from set_method here, returning it
        // silently told the caller the attempt succeeded
        return ESP_FAIL;
    }

    int version = s_version;
    int64_t len = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);

    // framing (07:31): HTTP 200, X-Graph-Version not 0, body n*n
    if (status != 200 || version <= 0 || len != (int64_t)(GRAPH_SIZE)) {
        ESP_LOGE(TAG, "invalid response: status=%d v=%d len=%lld", status, version, (long long)len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "fetch: status=%d len=%lld", status, (long long)len);
    uint8_t buf[GRAPH_SIZE];

    int total = 0;
    int read_len;
    while (total < (int)len && (read_len = esp_http_client_read(client, (char*)(buf + total), (int)len - total)) > 0) {
        total += read_len;
        taskYIELD(); // cegah IDLE0 kelaparan CPU saat read beruntun
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "fetch: read %d / %lld byte", total, (long long)len);

    if (total != (int)len) {
        ESP_LOGE(TAG, "partial read: %d / %lld", total, (long long)len);
        return ESP_ERR_HTTP_INCOMPLETE_DATA;
    }

    err = graph_update(buf, (uint32_t)version);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "graph_update failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "graph v%d installed: %lld bytes", version, (long long)len);
    return ESP_OK;

nvs_close_on_error:
    nvs_close(nvs_handle);
    return ret;
}

// Only ever called from graph_fetch_task(): the TLS handshake needs ~8 KB, so
// this must not run on the caller's stack (see graph_fetch_start_task()).
static bool graph_fetch_run(void) {
    // Do not touch the socket before WiFi has an IP. Bound this wait so the
    // caller can settle status to ERROR when the AP never connects.
    if (!wifi_wait_connected(FETCH_WIFI_WAIT_MS)) {
        ESP_LOGE(TAG, "WiFi did not obtain an IP within %d ms; fetch failed", FETCH_WIFI_WAIT_MS);
#ifdef ADAM_GRAPH_FALLBACK_DUMMY
        ESP_LOGW(TAG, "DEV: no WiFi, falling back to the dummy graph");
        if (s_fetch_event_group != NULL && graph_update(dummy_graph, 1) == ESP_OK) {
            xEventGroupSetBits(s_fetch_event_group, FETCH_SUCCESS_BIT);
            return true;
        }
#endif
        if (s_fetch_event_group != NULL) {
            xEventGroupSetBits(s_fetch_event_group, FETCH_ERROR_BIT);
        }
        return false;
    }

    for (int attempt = 1; attempt <= FETCH_RETRY_COUNT; attempt++) {
        if (fetch_and_apply() == ESP_OK) {
            if (s_fetch_event_group != NULL) {
                xEventGroupSetBits(s_fetch_event_group, FETCH_SUCCESS_BIT);
            }
            return true;
        }
        ESP_LOGW(TAG, "attempt %d/%d failed", attempt, FETCH_RETRY_COUNT);
        if (attempt < FETCH_RETRY_COUNT) {
            vTaskDelay((TickType_t)pdMS_TO_TICKS(FETCH_RETRY_DELAY_MS));
        }
    }

#ifdef ADAM_GRAPH_FALLBACK_DUMMY
    if (graph_update(dummy_graph, 1) == ESP_OK) {
        ESP_LOGW(TAG, "DEV: fetch failed, installed dummy graph v1");
        if (s_fetch_event_group != NULL) {
            xEventGroupSetBits(s_fetch_event_group, FETCH_SUCCESS_BIT);
        }
        return true;
    }
#endif
    // 06 §6.4 / 07 §7.3.2: graph_version stays 0, no retry until admin reset
    // or reboot
    if (s_fetch_event_group != NULL) {
        xEventGroupSetBits(s_fetch_event_group, FETCH_ERROR_BIT);
    }
    return false;
}

// Spec 07 §7.1: Task Fetch, low priority (1), core 0. Runs once at boot and
// then deletes itself, so no idle task is left behind.
static void graph_fetch_task(void* pvParameters) {
    graph_fetch_run();

    // One-shot: fetch settled (success or out of retries), release the stack
    vTaskDelete(NULL);
}

// 8192: esp_http_client + the TLS handshake need about that much - far more
// than CONFIG_ESP_MAIN_TASK_STACK_SIZE (3584) or the 4096 of Task Executor,
// hence the dedicated task. The event group is created here, not in the task:
// the executor and MQTT already query the fetch state before this task gets
// its first slice of CPU.
static esp_err_t graph_fetch_start_task(void) {
    if (s_fetch_event_group == NULL) {
        s_fetch_event_group = xEventGroupCreate();
        if (s_fetch_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Cleared before the task starts, so graph_fetch_success() waiting for the
    // next result can never read the outcome of the previous fetch.
    xEventGroupClearBits(s_fetch_event_group, FETCH_SUCCESS_BIT | FETCH_ERROR_BIT);

    if (xTaskCreatePinnedToCore(graph_fetch_task, "Task Fetch", 8192, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t graph_fetch_init(void) {
    return graph_fetch_start_task();
}

bool graph_fetch_rerun(void) {
    if (graph_fetch_start_task() != ESP_OK) {
        return false;
    }
    bool ok = graph_fetch_success();
    // The reset published version 0 before the fetch; report what was actually
    // installed (a failed fetch keeps 0, 07:39,67). At boot the client does not
    // exist yet, so the connect handler is the one that publishes.
    if (ok) {
        mqtt_publish_graph_version(graph_version());
    }
    return ok;
}
