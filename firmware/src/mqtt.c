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
#include "graph.h"
#include "graph_fetch.h"
#include "mqtt_client.h"
#include "path_resolver.h"
#include "portmacro.h"
#include "sdkconfig.h"
#include "sensors.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#    include "esp_crt_bundle.h"
#endif

static const char* TAG = "mqtt";

// Interval of the periodic battery/status/pos/trash telemetry (3 seconds)
#define TELEMETRY_INTERVAL_MS 3000

static QueueHandle_t s_job_queue;
static QueueHandle_t s_event_data_queue;

static esp_mqtt_client_handle_t s_client;

// Telemetry task, low priority so it does not disturb the MQTT socket task
static void telemetry_task(void* pvParameters) {
    for (;;) {
        uint8_t battery = sensors_battery_percent();
        uint8_t trash = sensors_trash_level_percent();
        char pos = (char)(path_resolver_current() + 'A');
        uint8_t status = (uint8_t)executor_status();

        // docs/06-protocol.md: raw 1-byte payloads, battery/status/pos/trash
        // retained true, QoS 1
        mqtt_publish(MQTT_BATTERY_TOPIC, (const char*)&battery, 1, 1, true);
        mqtt_publish(MQTT_TRASH_TOPIC, (const char*)&trash, 1, 1, true);
        mqtt_publish(MQTT_STATUS_TOPIC, (const char*)&status, 1, 1, true);
        mqtt_publish(MQTT_CURRENT_POS_TOPIC, &pos, 1, 1, true);

        ESP_LOGI(TAG, "Telemetry: pos=%c status=%d battery=%d%% trash=%d%%", pos, status, battery, trash);

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}

/* MQTT_EVENT_DATA and its topic/data buffers are owned by the esp-mqtt task
 * and recycled the moment this handler returns, so the validation task must
 * never hold a pointer into them: every byte it needs is copied into this
 * queue-safe value first. */
#define MQTT_MSG_TOPIC_MAX 32
#define MQTT_MSG_DATA_MAX 4 /* docs/06-protocol.md:345 - command payload max 3 byte */

typedef struct {
    char topic[MQTT_MSG_TOPIC_MAX];
    int topic_len;
    char data[MQTT_MSG_DATA_MAX];
    int data_len;
} mqtt_msg_t;

static inline bool cmp_topic(const mqtt_msg_t* msg, const char* topic) {
    return msg->topic_len == (int)strlen(topic) && memcmp(msg->topic, topic, (size_t)msg->topic_len) == 0;
}

/* req_id on the wire is 2 byte little-endian (docs/06-protocol.md:342,
 * example moveto 34 12 43 -> req_id 0x1234, node 'C'). The parser and
 * publish_ack() are the only places that touch the wire format, so both
 * directions of the round trip change together here. */
static uint16_t parse_req_id(const char* data) {
    return (uint16_t)((uint8_t)data[0] | ((uint16_t)(uint8_t)data[1] << 8));
}

/* Single writer for adam/srv/ack: [req_id:2 little-endian][result:1], QoS 1,
 * retain false (docs/06-protocol.md:354 - commands are never retained). */
static void publish_ack(uint16_t req_id, adam_ack_t result) {
    uint8_t payload[3];
    payload[0] = (uint8_t)(req_id & 0xff);
    payload[1] = (uint8_t)(req_id >> 8);
    payload[2] = (uint8_t)result;
    mqtt_publish(MQTT_ACK_TOPIC, (const char*)payload, 3, 1, false);
}

/* Dedup cache of accepted (req_id, node) pairs - docs/07-firmware.md:43: a
 * QoS 1 retransmission is answered DUPLICATE instead of running the job
 * twice, and that check runs before BUSY. Dropped on every connect
 * (docs/07-firmware.md:122) because clean session is true. Ring buffer, the
 * oldest pair is overwritten; req_id 0x0000 is never stored because it is
 * reserved (docs/07-firmware.md:121). Written by the MQTT event task on
 * reconnect and by the validation task, hence the spinlock. */
#define DEDUP_CACHE_SIZE 8

typedef struct {
    uint16_t req_id;
    char node;
    bool used;
} dedup_entry_t;

static dedup_entry_t s_dedup[DEDUP_CACHE_SIZE];
static size_t s_dedup_next = 0;
static portMUX_TYPE s_dedup_mux = portMUX_INITIALIZER_UNLOCKED;

static bool dedup_contains(uint16_t req_id, char node) {
    bool found = false;
    portENTER_CRITICAL(&s_dedup_mux);
    for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
        if (s_dedup[i].used && s_dedup[i].req_id == req_id && s_dedup[i].node == node) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_dedup_mux);
    return found;
}

static void dedup_remember(uint16_t req_id, char node) {
    if (req_id == 0x0000) {
        return;
    }
    portENTER_CRITICAL(&s_dedup_mux);
    s_dedup[s_dedup_next] = (dedup_entry_t){.req_id = req_id, .node = node, .used = true};
    s_dedup_next = (s_dedup_next + 1) % DEDUP_CACHE_SIZE;
    portEXIT_CRITICAL(&s_dedup_mux);
}

static void dedup_clear(void) {
    portENTER_CRITICAL(&s_dedup_mux);
    memset(s_dedup, 0, sizeof(s_dedup));
    s_dedup_next = 0;
    portEXIT_CRITICAL(&s_dedup_mux);
}

/* docs/07-firmware.md:44 - "graf belum siap". There is no graph_version topic
 * yet (TODO.md item 7), so readiness is taken from graph_get(): false until
 * Task Fetch installs the matrix, and after a failed fetch. */
static bool graph_ready(void) {
    const uint8_t* edges;
    size_t count;
    return graph_get(&edges, &count);
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        // docs/07-firmware.md:122 - the dedup cache never survives a reconnect
        dedup_clear();

        int msg_id = esp_mqtt_client_subscribe(client, MQTT_MOVE_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe moveto, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, MQTT_RESET_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe reset, msg_id=%d", msg_id);

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
    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);

        // Copy the bytes out of the event: queueing the event pointer itself
        // would store the first word of the struct, not the pointer.
        mqtt_msg_t msg = {
            .topic_len = event->topic_len,
            .data_len = event->data_len,
        };
        if (msg.topic_len > (int)sizeof(msg.topic)) {
            msg.topic_len = (int)sizeof(msg.topic);
        }
        if (msg.data_len > (int)sizeof(msg.data)) {
            msg.data_len = (int)sizeof(msg.data);
        }
        if (msg.topic_len > 0) {
            memcpy(msg.topic, event->topic, (size_t)msg.topic_len);
        }
        if (msg.data_len > 0) {
            memcpy(msg.data, event->data, (size_t)msg.data_len);
        }

        // A command must never vanish silently: wait for the validation task
        // and, if it still cannot be queued, answer QUEUE_FULL so the gateway
        // does not have to sit out its ack timeout (docs/06-protocol.md:281).
        if (xQueueSend(s_event_data_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "Validation queue full, dropped %.*s", msg.topic_len, msg.topic);
            if (cmp_topic(&msg, MQTT_MOVE_TOPIC) && msg.data_len >= 2) {
                publish_ack(parse_req_id(msg.data), ADAM_ACK_QUEUE_FULL);
            }
        }
        break;
    }
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
        break;
    }
}

static void event_validation_task(void* pvParameters) {
    for (;;) {
        mqtt_msg_t msg;
        if (xQueueReceive(s_event_data_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (cmp_topic(&msg, MQTT_MOVE_TOPIC)) {
                uint16_t req_id = msg.data_len >= 2 ? parse_req_id(msg.data) : 0;

                if (msg.data_len < 3) {
                    // docs/06-protocol.md:377 - an invalid payload is answered
                    // with INVALID_TARGET; returning here would tear down the
                    // task and swallow every command after it.
                    publish_ack(req_id, ADAM_ACK_INVALID_TARGET);
                    continue;
                }

                adam_ack_t ack_status = ADAM_ACK_ACCEPTED;
                mqtt_job_t job = {
                    .req_id = req_id,
                    .node = msg.data[2],
                    .reset = false,
                };

                // docs/07-firmware.md:43 - dedup before everything else but the
                // payload length, so a QoS 1 retry is never taken for a new job
                if (job.req_id != 0x0000 && dedup_contains(job.req_id, job.node)) {
                    publish_ack(job.req_id, ADAM_ACK_DUPLICATE);
                    continue;
                }

                // docs/07-firmware.md:44 - status ERROR or graph not ready yet
                // (boot, or fetch failed) is NOT_READY, not BUSY
                if (executor_status() == ADAM_STATUS_ERROR || !graph_ready()) {
                    publish_ack(job.req_id, ADAM_ACK_NOT_READY);
                    continue;
                }

                if (job.node < 'A' || job.node > 'Z') {
                    ack_status = ADAM_ACK_INVALID_TARGET;
                    goto publish;
                }
                // docs/07-firmware.md:46 - not IDLE, or the 1-slot mailbox
                // still holds the previous job: BUSY lets the gateway requeue
                // the request instead of failing it. Only this task writes to
                // the mailbox, so a later xQueueSend failure really is the
                // internal safety net (docs/06-protocol.md:375).
                if (executor_status() != ADAM_STATUS_IDLE || uxQueueMessagesWaiting(s_job_queue) > 0) {
                    ack_status = ADAM_ACK_BUSY;
                    goto publish;
                }
                uint8_t trash = sensors_trash_level_percent();
                if (trash >= SENSORS_TRASH_FULL_PERCENT) {
                    ack_status = ADAM_ACK_TRASH_FULL;
                    mqtt_publish(MQTT_TRASH_TOPIC, (const char*)&trash, 1, 1, true);
                    goto publish;
                }
                if (xQueueSend(s_job_queue, &job, (TickType_t)0) != pdTRUE) {
                    ack_status = ADAM_ACK_QUEUE_FULL;
                    goto publish;
                }
            publish:
                if (ack_status == ADAM_ACK_ACCEPTED) {
                    dedup_remember(job.req_id, job.node);
                }
                publish_ack(job.req_id, ack_status);

            } else if (cmp_topic(&msg, MQTT_RESET_TOPIC)) {
                // Spec 6.2: admin reset, payload node ('A' default). Flush the
                // remaining commands so the recovery route stays clean.
                mqtt_flush_jobs();
                mqtt_job_t job = {
                    .req_id = msg.data_len >= 2 ? parse_req_id(msg.data) : 0,
                    .node = msg.data_len >= 3 ? msg.data[2] : 'A',
                    .reset = true,
                };
                xQueueSend(s_job_queue, &job, (TickType_t)0);
            }
        }
    }
}

esp_err_t mqtt_app_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker =
            {
                .address.uri = MQTT_BROKER_URI,

                .verification =
                    {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
                        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
                    },
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

    s_event_data_queue = xQueueCreate(2, sizeof(mqtt_msg_t));
    if (s_event_data_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_job_queue = xQueueCreate(1, sizeof(mqtt_job_t));
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

    if (xTaskCreatePinnedToCore(event_validation_task, "Event Validation", 3072, NULL, 1, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

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
