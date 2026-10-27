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
#include "sdkconfig.h"
#include "wifi.h"
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#    include "esp_crt_bundle.h"
#endif

static const char* TAG = "graph_fetch";

// X-Graph-* response headers, captured in the event handler: IDF 5.x has no
// esp_http_client_get_response_header yet
static struct {
    int version;
    int nodes;
} s_meta;

static esp_err_t fetch_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "X-Graph-Nodes") == 0) {
            s_meta.nodes = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "X-Graph-Version") == 0) {
            s_meta.version = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

static void fetch_and_apply(void) {
    esp_http_client_config_t config = {
        .url = GRAPH_SERVER_URL,
        .timeout_ms = 5000,
        .event_handler = fetch_event_handler,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        // Public CA (e.g. Let's Encrypt), for a self signed cert use .cert_pem instead
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    s_meta.version = -1;
    s_meta.nodes = 0;

    ESP_LOGI(TAG, "fetch: open %s", GRAPH_SERVER_URL);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int n = s_meta.nodes;
    int version = s_meta.version;
    int64_t len = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);

    // framing (07:31): HTTP 200, X-Graph-Version not 0, body n*n
    if (status != 200 || version <= 0 || n <= 0 || len != (int64_t)(n * n)) {
        ESP_LOGE(TAG, "invalid response: status=%d n=%d v=%d len=%lld", status, n, version, (long long)len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    ESP_LOGI(TAG, "fetch: status=%d n=%d len=%lld", status, n, (long long)len);
    uint8_t* buf = malloc((size_t)len);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for %lld bytes", (long long)len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

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
        free(buf);
        return;
    }

    err = graph_update(buf, (size_t)n, (uint32_t)version);
    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "graph_update failed: %s", esp_err_to_name(err));
        return;
    }

    // real version out once the matrix is installed; a failed fetch keeps 0
    // (07:39,67)
    mqtt_publish_graph_version(graph_version());

    ESP_LOGI(TAG, "graph v%d installed: %d nodes, %lld bytes", version, n, (long long)len);
}

// Spec 7.1: Task Fetch, low priority (1), core 0. Downloaded once at boot and
// never refreshed at runtime (07:31) - so the task fetches, installs and exits.
static void graph_fetch_task(void* pvParameters) {
    // Do not touch the socket before WiFi has an IP, this transport state can
    // spin the CPU without a long yield until the task watchdog fires
    wifi_wait_connected();
    fetch_and_apply(); // initial download at boot
}

esp_err_t graph_fetch_init(void) {
    // 8192: esp_http_client + the TLS handshake need about that much
    if (xTaskCreatePinnedToCore(graph_fetch_task, "Task Fetch", 8192, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
