#include "executor.h"

#include "dwell.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "graph.h"
#include "motor.h"
#include "mqtt.h"
#include "path_resolver.h"
#include "protocol.h"
#include "sensors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char* TAG = "executor";

// s_state = what the task loop is doing, s_status = published wire code
// (adam_status_t). OBSTACLE is reserved and no longer emitted.
typedef enum {
    STATE_IDLE,
    STATE_MOVING,
    STATE_ERROR,
} exec_state_t;

static volatile exec_state_t s_state = STATE_IDLE;
static volatile adam_status_t s_status = ADAM_STATUS_IDLE;

// PID line follower constants (base speed 160, Kp 45, Kd 18)
#define PID_BASE_SPEED 160
#define PID_KP 45.0f
#define PID_KD 18.0f
#define PID_LOOP_MS 10 // 100 Hz control loop (spec 7.1)

// Node detection: the 5-IR junction must be seen several times in a row
#define NODE_JUNCTION_RUNS 3
#define LINE_PATTERN_ALL 0b111
// Navigation errors: line lost while MOVING (spec 7.2) and step timeout
#define LINE_LOST_MS 500
#define STEP_TIMEOUT_MS 30000

static nav_path_t s_nav;
static size_t s_step = 0;

static float s_last_error = 0.0f;

// Junction counter of the node detection (spec 2.2)
static int s_junction_run = 0;
// Color marker validation of the junction that is currently counted
static bool s_junction_valid = false;

static inline int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static int64_t s_step_deadline = 0;
static int64_t s_no_line_since = -1;
// Anti false-arrival latch: after an arrival the junction has to leave the
// sensor view before the next node may be detected
static bool s_wait_clear = false;

// Serial log name only - never sent on the wire (docs/06-protocol.md)
static const char* err_name(adam_error_t err) {
    switch (err) {
    case ADAM_ERR_NO_PATH:
        return "NO_PATH";
    case ADAM_ERR_LINE_LOST:
        return "LINE_LOST";
    case ADAM_ERR_NODE_TIMEOUT:
        return "NODE_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

adam_status_t executor_status(void) {
    return s_status;
}

// Publish the status, but only when it actually changed
static void publish_status(adam_status_t status) {
    static int s_published = -1;
    if (s_published == (int)status) {
        return;
    }
    s_published = (int)status;
    uint8_t wire = (uint8_t)status;
    mqtt_publish(MQTT_STATUS_TOPIC, (const char*)&wire, 1, 1, true);
}

// current_pos = [current][next], 2 byte retain; next = 0 when heading nowhere
static void publish_current_pos(size_t current, size_t next) {
    uint8_t payload[2] = {(uint8_t)(current + 'A'), (uint8_t)(next ? next + 'A' : 0)};
    mqtt_publish(MQTT_CURRENT_POS_TOPIC, (const char*)payload, 2, 1, true);
    ESP_LOGI(TAG, "Position published: %c -> %c", payload[0], payload[1] ? payload[1] : '-');
}

// trash right after the lid closes (07:107, 04:14); at boot via MQTT connect
static void publish_trash(void) {
    uint8_t trash = sensors_trash_level_percent();
    mqtt_publish(MQTT_TRASH_TOPIC, (const char*)&trash, 1, 1, true);
}

void executor_set_error(uint16_t req_id, adam_error_t reason) {
    if (s_state == STATE_ERROR) {
        return;
    }
    s_state = STATE_ERROR;

    // Spec 7.2: stop the motor and flush every queue
    motor_halt();
    path_resolver_flush_paths();
    mqtt_flush_jobs();

    // [req_id:2 LE][code:1]; the id comes from the caller so it can never be
    // stale (06:348), 0 for an error that belongs to no request (07:76)
    adam_req_t data = {
        .id = req_id,
        .code = reason,
    };
    mqtt_publish(MQTT_ERROR_TOPIC, (const char*)&data, 3, 1, false);
    // current_pos = [current][0] before status = ERROR (07:76)
    publish_current_pos(path_resolver_current(), 0);
    publish_status(ADAM_STATUS_ERROR);
    s_status = ADAM_STATUS_ERROR;
    ESP_LOGE(TAG, "ERROR: %s, robot stopped and waits for an admin reset (spec 7.2)", err_name(reason));
}

// Turn onto the edge direction and reset the deadline (spec 3.2). Returns false
// when the edge is not valid in the current graph.
static bool move_begin_step(void) {
    size_t from = s_nav.path[s_step - 1];
    size_t to = s_nav.path[s_step];

    const uint8_t* edges;
    size_t count;
    if (!graph_get(&edges, &count) || from >= count || to >= count) {
        return false;
    }

    uint8_t edge = edges[(from * count) + to];
    if (EDGE_WEIGHT(edge) == NO_EDGE) {
        return false;
    }

    motor_turn_to_heading(EDGE_DIR(edge));

    // current_pos = [from][to] at every edge start (07:58)
    publish_current_pos(from, to);

    s_step_deadline = now_ms() + STEP_TIMEOUT_MS;
    s_no_line_since = -1;
    s_last_error = 0.0f;

    ESP_LOGI(TAG, "Step %d/%d: %c -> %c dir %s", (int)s_step, (int)s_nav.len - 1, (int)(from + 'A'), (int)(to + 'A'),
             STR_DIR[EDGE_DIR(edge)]);
    return true;
}

// Line pattern summary: 3-bit mask of the BFD-1000 channels (bit0=left, bit1=mid, bit2=right)
static inline int line_pattern(const int line[SENSORS_LINE_COUNT]) {
    int pattern = 0;
    for (int i = 0; i < SENSORS_LINE_COUNT; i++) {
        pattern |= (line[i] & 1) << i;
    }
    return pattern;
}

// Line error -1..1 from the active sensor positions (-2..+2, normalized by 2)
static float line_error(const int line[SENSORS_LINE_COUNT]) {
    static const int weight[SENSORS_LINE_COUNT] = {-2, 0, 2};
    int sum = 0;
    int count = 0;

    for (int i = 0; i < SENSORS_LINE_COUNT; i++) {
        if (line[i]) {
            sum += weight[i];
            count++;
        }
    }
    if (count == 0) {
        return s_last_error;
    }
    return ((float)sum / (float)count) / 2.0f;
}

// One MOVING iteration: PID, node detection, LINE_LOST, timeout
static void moving_tick(void) {
    int line[SENSORS_LINE_COUNT];
    sensors_line_read(line);
    int pattern = line_pattern(line);

    // LINE_LOST (spec 7.2): no sensor sees the line
    if (pattern == 0) {
        if (s_no_line_since < 0) {
            s_no_line_since = now_ms();
        } else if (now_ms() - s_no_line_since > LINE_LOST_MS) {
            ESP_LOGE(TAG, "Line lost (5 sensors blank > %d ms)", LINE_LOST_MS);
            executor_set_error(s_nav.job.req.id, ADAM_ERR_LINE_LOST);
            return;
        }
    } else {
        s_no_line_since = -1;
    }

    // PID line follower: weighted line error -1..1 with differential steering
    float error = line_error(line);

    float p = PID_KP * error;
    float d = PID_KD * (error - s_last_error);
    s_last_error = error;

    motor_set_speed(PID_BASE_SPEED - (int)(p + d), PID_BASE_SPEED + (int)(p + d));

    // Node arrival detection (spec 2.2): all 5 BFD-1000 sensors see the line at
    // the same time (junction), validated by the TCS3200 color marker.
    // After one arrival the junction has to leave the sensors first (s_wait_clear).
    bool node_detected = false;

    if (s_wait_clear && pattern != LINE_PATTERN_ALL) {
        s_wait_clear = false;
    }

    if (!s_wait_clear && pattern == LINE_PATTERN_ALL) {
        if (s_junction_run == 0) {
            s_junction_valid = sensors_color_marker_present();
        }
        s_junction_run++;
        if (s_junction_run >= NODE_JUNCTION_RUNS) {
            node_detected = true;
        }
    } else if (pattern != LINE_PATTERN_ALL) {
        s_junction_run = 0;
        s_junction_valid = false;
    }

    if (!node_detected) {
        // Step timeout: the node was not reached
        if (now_ms() > s_step_deadline) {
            ESP_LOGE(TAG, "Step %d/%d not finished within %d ms", (int)s_step, (int)s_nav.len - 1, STEP_TIMEOUT_MS);
            executor_set_error(s_nav.job.req.id, ADAM_ERR_NODE_TIMEOUT);
            return;
        }
        return; // not there yet, keep running the PID
    }

    // Valid node arrival
    motor_halt();
    ESP_LOGI(TAG, "NODE DETECTED (marker: %s)", s_junction_valid ? "TCS3200 color" : "5-IR junction fallback");

    size_t to = s_nav.path[s_step];
    s_junction_run = 0;
    s_junction_valid = false;
    s_wait_clear = true;
    path_resolver_set_current(to);
    s_step++;

    // current_pos = [node][next], next = 0 at the target (07:59)
    publish_current_pos(to, s_step < s_nav.len ? s_nav.path[s_step] : 0);

    // Home (Node A): face north again (spec 3.1 and 7.2)
    if (to == 0) {
        motor_turn_to_heading(DIR_NORTH);
    }

    if (s_step >= s_nav.len) {
        // Path finished
        s_junction_run = 0;

        if (!s_nav.job.reset) {
            // Dwell on the target node (spec 4) after a normal ride
            publish_current_pos(path_resolver_current(), 0);
            publish_status(ADAM_STATUS_DWELL);
            s_status = ADAM_STATUS_DWELL;
            dwell_run();
            publish_trash();
        }
        s_status = ADAM_STATUS_IDLE;
        publish_status(ADAM_STATUS_IDLE);
        publish_current_pos(path_resolver_current(), 0);
        s_state = STATE_IDLE;
        ESP_LOGI(TAG, "%s navigation finished, back to IDLE", s_nav.job.reset ? "Recovery" : "Normal");
        return;
    }

    // Continue with the next edge
    if (!move_begin_step()) {
        executor_set_error(s_nav.job.req.id, ADAM_ERR_NO_PATH);
        return;
    }
}

// One IDLE iteration: take a path from the Route Finder and start navigating
static void idle_tick(void) {
    nav_path_t nav;
    if (!path_resolver_get_nav(&nav)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        return;
    }

    if (nav.len == 0 || nav.len > NAV_MAX_NODES) {
        ESP_LOGW(TAG, "Invalid path (len=%d), dropped", (int)nav.len);
        return;
    }

    s_nav = nav;
    s_step = 1;
    s_wait_clear = true; // do not detect a node while still on the start node

    if (s_nav.len == 1) {
        // Target = current position: dwell right away (spec 4), without moving
        publish_current_pos(s_nav.path[0], 0);
        publish_status(ADAM_STATUS_DWELL);
        s_status = ADAM_STATUS_DWELL;
        dwell_run();
        publish_trash();
        s_status = ADAM_STATUS_IDLE;
        publish_status(ADAM_STATUS_IDLE);
        publish_current_pos(s_nav.path[0], 0);
        ESP_LOGI(TAG, "Already on the target node, dwell finished");
        return;
    }

    s_status = ADAM_STATUS_MOVING;
    publish_status(ADAM_STATUS_MOVING);
    if (!move_begin_step()) {
        executor_set_error(nav.job.req.id, ADAM_ERR_NO_PATH);
    }
}

static void executor(void* pvParameters) {
    ESP_LOGI(TAG, "Task Executor ON: PID 100 Hz (Kp=%d, Kd=%d), core 1, prio 3", (int)PID_KP, (int)PID_KD);

    publish_status(ADAM_STATUS_IDLE);

    for (;;) {
        switch (s_state) {
        case STATE_MOVING:
            moving_tick();
            vTaskDelay(pdMS_TO_TICKS(PID_LOOP_MS));
            break;

        case STATE_IDLE:
            idle_tick();
            break;

        case STATE_ERROR:
            // Spec 7.2: the robot stays on current_node and waits for the admin.
            // Only a 'recovery' path (from the reset command) is accepted.
            motor_halt();
            {
                nav_path_t nav;
                bool got = path_resolver_get_nav(&nav);
                if (got && nav.job.reset && nav.len >= 1) {
                    ESP_LOGW(TAG, "Recovery: admin reset route accepted (%d nodes)", (int)nav.len);
                    s_nav = nav;
                    s_wait_clear = true;

                    if (nav.len == 1) { // Reset while already on Home: only fix the heading
                        motor_turn_to_heading(DIR_NORTH);
                        s_status = ADAM_STATUS_IDLE;
                        publish_status(ADAM_STATUS_IDLE);
                        publish_current_pos(path_resolver_current(), 0);
                        s_state = STATE_IDLE;
                        ESP_LOGI(TAG, "Recovery finished (already on Home)");
                        break;
                    }

                    s_step = 1;
                    s_state = STATE_MOVING;
                    s_status = ADAM_STATUS_MOVING;
                    publish_status(ADAM_STATUS_MOVING);
                    if (!move_begin_step()) {
                        executor_set_error(nav.job.req.id, ADAM_ERR_NO_PATH);
                    }
                } else if (got) {
                    ESP_LOGW(TAG, "Non recovery path ignored while in ERROR");
                } else {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            break;

        default:
            break;
        }
    }
}

esp_err_t executor_init(void) {
    if (xTaskCreatePinnedToCore(executor, "Task Executor", 4096, NULL, 3, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
