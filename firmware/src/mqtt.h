#ifndef SRC_MQTT_H
#define SRC_MQTT_H

#include "esp_err.h"
#include "protocol.h"
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
#define MQTT_CURRENT_POS_TOPIC "adam/srv/current_pos"     // [current:1][next:1], next = 0
#define MQTT_STATUS_TOPIC "adam/srv/status"               // 1 byte adam_status_t
#define MQTT_BATTERY_TOPIC "adam/srv/battery"             // 1 byte 0..100
#define MQTT_TRASH_TOPIC "adam/srv/trash"                 // 1 byte 0..100 (100 = full)
#define MQTT_ERROR_TOPIC "adam/srv/error"                 // [req_id:2][code:1], no retain
#define MQTT_GRAPH_VERSION_TOPIC "adam/srv/graph_version" // [version:4] little-endian, retain
#define MQTT_ONLINE_TOPIC "adam/srv/online"               // 0x00/0x01, retain, LWT 0x00

// Wire types (adam_status_t, adam_ack_t, adam_error_t, adam_req_t, adam_job_t)
// live in protocol.h: they belong to the protocol, not to this transport.

esp_err_t mqtt_app_start(void);
void mqtt_publish(const char* topic, const char* data, int len, int qos, int retain);

// Publish a 1-byte payload (battery, trash, status, online heartbeat)
void mqtt_publish_u8(const char* topic, uint8_t data, int qos, int retain);

// adam/srv/graph_version: 4 byte little-endian, retained, 0 = not ready
// (the value itself is owned by graph.c, graph_version())
void mqtt_publish_graph_version(uint32_t version);

// Get the next command (move / reset), blocking, called by the Route Finder
bool mqtt_get_job_blocked(adam_job_t* job);

// Flush the command queue (called on error recovery, spec 7.2)
void mqtt_flush_jobs(void);

#endif // SRC_MQTT_H
