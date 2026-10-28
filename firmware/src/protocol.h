#ifndef SRC_PROTOCOL_H
#define SRC_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* Wire types of the protocol (docs/06-protocol.md), shared by executor, Task
 * Route Finder and MQTT. Topics + publish API stay in mqtt.h. Payloads are raw
 * bytes: append-only codes, 0 = NONE/UNKNOWN, unknown codes are ignored. */

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
    // GET /graph.bin failed after all retries (boot or reset); version stays 0
    // (06 §6.4, append-only)
    ADAM_ERR_FETCH_FAILED = 5,
} adam_error_t;

// [req_id:2 little-endian][byte:1]; req_id 0x0000 is reserved (06-protocol.md)
typedef struct {
    uint16_t id;
    union {
        uint8_t data;
        uint8_t result; // ack: [req_id][result]
        uint8_t code;   // error: [req_id][code]
        uint8_t node;   // moveto: [req_id][node]
    };
} adam_req_t;

// One command from the gateway: target node (docs/06-protocol.md). internal =
// firmware-generated auto-park to Home when the bin is full (07 §7.6): no
// ack/req_id is sent for it and the robot skips the dwell at Home.
typedef struct {
    adam_req_t req;
    bool internal;
} adam_job_t;

#endif // SRC_PROTOCOL_H