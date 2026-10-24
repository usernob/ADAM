#include "mqtt.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "executor.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "graph_fetch.h"
#include "mqtt_cert.h"
#include "mqtt_client.h"
#include "path_resolver.h"
#include "portmacro.h"
#include "sensors.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char* TAG = "mqtt";

// Interval of the periodic battery/status/pos/trash telemetry (3 seconds)
#define TELEMETRY_INTERVAL_MS 3000

static QueueHandle_t s_job_queue;

// Telemetry task, low priority so it does not disturb the MQTT socket task
static void telemetry_task(void* pvParameters) {
    for (;;) {
        uint8_t battery = sensors_battery_percent();
        uint8_t trash = sensors_trash_level_percent();
        char pos = (char)(path_resolver_current() + 'A');
        uint8_t status = (uint8_t)executor_status();

        // docs/06-protokol.md: raw 1-byte payloads, battery/status/pos/trash
        // retained true, QoS 1
        mqtt_publish(MQTT_BATTERY_TOPIC, (const char*)&battery, 1, 1, true);
        mqtt_publish(MQTT_TRASH_TOPIC, (const char*)&trash, 1, 1, true);
        mqtt_publish(MQTT_STATUS_TOPIC, (const char*)&status, 1, 1, true);
        mqtt_publish(MQTT_CURRENT_POS_TOPIC, &pos, 1, 1, true);

        ESP_LOGI(TAG, "Telemetry: pos=%c status=%d battery=%d%% trash=%d%%", pos, status, battery, trash);

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

static inline bool cmp_topic(const esp_mqtt_event_handle_t event, const char* topic) {
    return event->topic_len == strlen(topic) && memcmp(event->topic, topic, event->topic_len) == 0;
}
static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        int msg_id = esp_mqtt_client_subscribe(client, MQTT_MOVE_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe moveto, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, MQTT_RESET_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe reset, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, MQTT_REFRESHGRAPH_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe refreshgraph, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            ESP_LOGE(TAG, "subscribe rejected by the broker (msg_id=%d)", event->msg_id);
        } else {
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        }
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

        if (cmp_topic(event, MQTT_MOVE_TOPIC)) {
            if (event->data_len >= 1) {
                mqtt_job_t job = {
                    .node = event->data[0],
                    .reset = false,
                };
                xQueueSend(s_job_queue, &job, (TickType_t)0);
            }
        } else if (cmp_topic(event, MQTT_RESET_TOPIC)) {
            // Spec 6.2: admin reset, payload node ('A' default). Flush the
            // remaining commands so the recovery route stays clean.
            mqtt_flush_jobs();
            mqtt_job_t job = {
                .node = event->data_len >= 1 ? event->data[0] : 'A',
                .reset = true,
            };
            xQueueSend(s_job_queue, &job, (TickType_t)0);
        } else if (cmp_topic(event, MQTT_REFRESHGRAPH_TOPIC)) {
            graph_fetch_notify();
        }

        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGE(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static esp_mqtt_client_handle_t s_client;

esp_err_t mqtt_app_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker =
            {
                .address.uri = MQTT_BROKER_URI,
                .verification.certificate = (const char*)mqtt_cert_pem,
                .verification.certificate_len = sizeof(mqtt_cert_pem),
            },
        .credentials =
            {
                .client_id = "adam-robot",
                .username = MQTT_USERNAME,
                .authentication.password = MQTT_PASSWORD,
            },
        .session =
            {
                .keepalive = 30,
                .disable_clean_session = true,
            },
        .task =
            {
                .priority = 2,
            },
    };

    s_job_queue = xQueueCreate(10, sizeof(mqtt_job_t));
    if (s_job_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL), TAG,
                        "Failed to register mqtt task");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "Failed to start mqtt");

    if (xTaskCreatePinnedToCore(telemetry_task, "Task Telemetry", 3072, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void mqtt_publish(const char* topic, const char* data, int len, int qos, int retain) {
    if (s_client == NULL) {
        return;
    }
    esp_mqtt_client_publish(s_client, topic, data, len, qos, retain);
}

bool mqtt_get_job_blocked(mqtt_job_t* job) {
    return xQueueReceive(s_job_queue, job, portMAX_DELAY) == pdTRUE;
}

void mqtt_flush_jobs(void) {
    if (s_job_queue != NULL) {
        xQueueReset(s_job_queue);
    }
}
