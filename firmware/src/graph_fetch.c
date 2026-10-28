#include "graph_fetch.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "graph.h"
#include "mqtt.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "wifi.h"
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#    include "esp_crt_bundle.h"
#endif

#define FETCH_RETRY_COUNT 3
#define FETCH_RETRY_DELAY_MS 2000 // 07:31 - jeda 2 s antar percobaan
#define FETCH_WIFI_WAIT_MS 30000  // batas tunggu IP sebelum boot fetch dinyatakan gagal

static const char* TAG = "graph_fetch";

static int s_version;

// Fetch failure state: set when all retries exhausted or WiFi timeout.
// The executor polls this to decide boot status (ERROR vs IDLE).
static volatile bool s_fetch_failed = false;

bool graph_fetch_has_failed(void) {
    return s_fetch_failed;
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
    esp_http_client_config_t config = {
        .url = GRAPH_SERVER_URL,
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

    ESP_LOGI(TAG, "fetch: open %s", GRAPH_SERVER_URL);
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

    // real version out once the matrix is installed; a failed fetch keeps 0
    // (07:39,67)
    mqtt_publish_graph_version(graph_version());

    ESP_LOGI(TAG, "graph v%d installed: %lld bytes", version, (long long)len);
    return ESP_OK;
}

// Spec 07 §7.1: Task Fetch, low priority (1), core 0. Runs once at boot and
// then deletes itself, so no idle task is left behind.
static void graph_fetch_task(void* pvParameters) {
    // Do not touch the socket before WiFi has an IP. Bound this wait so the
    // executor can settle boot status to ERROR when the AP never connects.
    if (!wifi_wait_connected(FETCH_WIFI_WAIT_MS)) {
        ESP_LOGE(TAG, "WiFi did not obtain an IP within %d ms; boot fetch failed", FETCH_WIFI_WAIT_MS);
        s_fetch_failed = true;
        vTaskDelete(NULL);
        return;
    }

    bool installed = false;
    for (int attempt = 1; attempt <= FETCH_RETRY_COUNT; attempt++) {
        if (fetch_and_apply() == ESP_OK) {
            installed = true;
            break;
        }
        ESP_LOGW(TAG, "attempt %d/%d failed", attempt, FETCH_RETRY_COUNT);
        if (attempt < FETCH_RETRY_COUNT) {
            vTaskDelay((TickType_t)pdMS_TO_TICKS(FETCH_RETRY_DELAY_MS));
        }
    }

    if (!installed) {
        // 06:317 / 07:38: graph_version stays 0, no retry until reboot, and
        // the boot status becomes ERROR instead of IDLE
        s_fetch_failed = true;
    }

    // One-shot: fetch settled (success or out of retries), release the stack
    vTaskDelete(NULL);
}

esp_err_t graph_fetch_init(void) {
    // 8192: esp_http_client + the TLS handshake need about that much - far
    // more than CONFIG_ESP_MAIN_TASK_STACK_SIZE (3584), hence the task
    if (xTaskCreatePinnedToCore(graph_fetch_task, "Task Fetch", 8192, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
