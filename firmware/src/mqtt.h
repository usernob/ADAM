#ifndef SRC_MQTT_H
#define SRC_MQTT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// MQTT config, currently just hardcoded for ease to use
#define MQTT_BROKER_URI "mqtts://s997b122.ala.us-east-1.emqxsl.com:8883"
#define MQTT_USERNAME "adam"
#define MQTT_PASSWORD "somethingsecret"

// Server -> robot topics (docs/06-protocol.md)
#define MQTT_MOVE_TOPIC "srv/adam/moveto"
#define MQTT_RESET_TOPIC "srv/adam/reset"

// Robot -> server topics (docs/06-protocol.md: QoS 1; state telemetry is
// retained, ack/error are not)
#define MQTT_ACK_TOPIC "adam/srv/ack"
#define MQTT_CURRENT_POS_TOPIC "adam/srv/current_pos" // 1 byte ASCII 'A'..
#define MQTT_STATUS_TOPIC "adam/srv/status"           // 1 byte adam_status_t
#define MQTT_BATTERY_TOPIC "adam/srv/battery"         // 1 byte 0..100
#define MQTT_TRASH_TOPIC "adam/srv/trash"             // 1 byte 0..100 (100 = full)
#define MQTT_ERROR_TOPIC "adam/srv/error"             // 1 byte adam_error_t, no retain
#define MQTT_ONLINE_TOPIC "adam/srv/online"

/* Wire enums (docs/06-protocol.md). Payloads are raw 1-byte enums, never
 * strings: machines talk to machines on MQTT, so a state change is a single
 * byte assignment instead of string copies/compares. Rules:
 * - append-only: never renumber an already released code
 * - 0 always means NONE/UNKNOWN
 * - receivers must ignore unknown codes (forward compatibility) */
typedef enum {
    ADAM_STATUS_IDLE = 0,
    ADAM_STATUS_MOVING = 1,
    ADAM_STATUS_DWELL = 2,
    ADAM_STATUS_ERROR = 3,
    // Reserved: was the internal BFD-1000 Near brake, no longer emitted
    ADAM_STATUS_OBSTACLE = 4,
} adam_status_t;

typedef enum {
    ADAM_ACK_ACCEPTED = 0,
    ADAM_ACK_BUSY = 1,
    ADAM_ACK_QUEUE_FULL = 2,
    ADAM_ACK_DUPLICATE = 3,
    ADAM_ACK_INVALID_TARGET = 4,
    ADAM_ACK_NOT_READY = 5,
    ADAM_ACK_TRASH_FULL = 6,
} adam_ack_t;

typedef enum {
    ADAM_ERR_NONE = 0,
    ADAM_ERR_NO_PATH = 1,
    ADAM_ERR_LINE_LOST = 2,
    ADAM_ERR_NODE_TIMEOUT = 3,
    ADAM_ERR_INVALID_TARGET = 4,
} adam_error_t;

typedef struct {
    uint16_t req_id;
    char node; // target node 'A'.. ('A' for a reset without payload)
    bool reset;
} mqtt_job_t;

esp_err_t mqtt_app_start(void);
void mqtt_publish(const char* topic, const char* data, int len, int qos, int retain);

// Get the next command (move / reset), blocking, called by the Route Finder
bool mqtt_get_job_blocked(mqtt_job_t* job);

// Flush the command queue (called on error recovery, spec 7.2)
void mqtt_flush_jobs(void);

#endif // SRC_MQTT_H
