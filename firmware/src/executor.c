#include "executor.h"

#include "dwell.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "graph.h"
#include "graph_fetch.h"
#include "motor.h"
#include "mqtt.h"
#include "path_resolver.h"
#include "portmacro.h"
#include "protocol.h"
#include "sensors.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char* TAG = "executor";

// s_state = what the task loop is doing, s_status = published wire code
// (adam_status_t). OBSTACLE is reserved and no longer emitted.
// STATE_DWELL keeps "serving at the lid" distinct from STATE_IDLE so internal
// code can tell an idle robot from one that is still in its dwell window.
typedef enum {
    STATE_IDLE,
    STATE_MOVING,
    STATE_DWELL,
    STATE_ERROR,
} exec_state_t;

static volatile exec_state_t s_state = STATE_IDLE;
static bool s_boot_settled = false;
typedef struct {
    adam_status_t status;
    uint8_t current_pos[2];
} executor_telemetry_t;
static portMUX_TYPE s_telemetry_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_telemetry_publish_mutex;
static executor_telemetry_t s_telemetry = {
    .status = ADAM_STATUS_IDLE,
    .current_pos = {'A', 0},
};
static int s_published_status = -1;
// next < 0 = no next node; see publish_state()
#define POS_NEXT_NONE (-1)
static void publish_state(size_t current, int next, adam_status_t status, bool boot_settled);

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
    portENTER_CRITICAL(&s_telemetry_mux);
    adam_status_t status = s_telemetry.status;
    portEXIT_CRITICAL(&s_telemetry_mux);
    return status;
}

bool executor_replay_telemetry(void) {
    if (s_telemetry_publish_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_telemetry_publish_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_telemetry_mux);
    if (!s_boot_settled) {
        portEXIT_CRITICAL(&s_telemetry_mux);
        xSemaphoreGive(s_telemetry_publish_mutex);
        return false;
    }
    executor_telemetry_t snapshot = s_telemetry;
    portEXIT_CRITICAL(&s_telemetry_mux);

    mqtt_publish(MQTT_CURRENT_POS_TOPIC, (const char*)snapshot.current_pos, sizeof(snapshot.current_pos), 1, true);
    mqtt_publish_u8(MQTT_STATUS_TOPIC, (uint8_t)snapshot.status, 1, true);
    xSemaphoreGive(s_telemetry_publish_mutex);
    return true;
}

// Boot status (07:38-39): nothing is published until Task Fetch settled -
// ERROR when every attempt failed, IDLE only once the graph is installed.
// Returns true when the boot decision is made and the task loop may run.
static bool boot_status_settle(void) {
    // Query graph_fetch's own state instead of having it push state to us.
    // graph_fetch owns the "fetch failed" state, so we pull it from there.
    if (graph_fetch_has_failed()) {
        // No error topic: there is no request behind a failed boot fetch, and
        // graph_version stays 0 (06:317). The robot takes no job until reboot.
        publish_state(path_resolver_current(), POS_NEXT_NONE, ADAM_STATUS_ERROR, true);
        ESP_LOGE(TAG, "Fetch failed after all retries, boot status = ERROR (07:38)");
        return true;
    }
    if (!graph_ready()) {
        return false;
    }
    publish_state(path_resolver_current(), POS_NEXT_NONE, ADAM_STATUS_IDLE, true);
    return true;
}

// current_pos = [current][next], 2 byte retain; next = 0 when heading nowhere.
// next is a node *index*, so it needs its own "none" value: 0 is node 'A', while
// the wire sentinel for "no next node" is 0x00 (06-protocol.md:349). Passing 0
// here would report "nowhere" whenever the robot heads for Home.
// The mutex serializes state updates with MQTT replay through both publish calls;
// the critical section protects the small shared snapshot itself.
static void publish_state(size_t current, int next, adam_status_t status, bool boot_settled) {
    uint8_t pos[2] = {(uint8_t)(current + 'A'), (uint8_t)(next >= 0 ? next + 'A' : 0)};
    bool status_changed;

    xSemaphoreTake(s_telemetry_publish_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_telemetry_mux);
    status_changed = s_published_status != (int)status;
    s_telemetry.status = status;
    s_telemetry.current_pos[0] = pos[0];
    s_telemetry.current_pos[1] = pos[1];
    if (boot_settled) {
        s_boot_settled = true;
    }
    if (status_changed) {
        s_published_status = (int)status;
    }
    portEXIT_CRITICAL(&s_telemetry_mux);

    mqtt_publish(MQTT_CURRENT_POS_TOPIC, (const char*)pos, sizeof(pos), 1, true);
    if (status_changed) {
        uint8_t wire = (uint8_t)status;
        mqtt_publish(MQTT_STATUS_TOPIC, (const char*)&wire, 1, 1, true);
    }
    ESP_LOGI(TAG, "State published: %c -> %c, status=%d", pos[0], pos[1] ? pos[1] : '-', (int)status);
    xSemaphoreGive(s_telemetry_publish_mutex);
}

// trash right after the lid closes (07:107, 04:14); at boot via MQTT connect
static void publish_trash(void) {
    uint8_t trash = sensors_trash_level_percent();
    mqtt_publish(MQTT_TRASH_TOPIC, (const char*)&trash, 1, 1, true);
}

/* One dwell on the target node (spec 4): publish DWELL, run the blocking dwell
 * window, then settle back to IDLE. The rollback is conditional because
 * executor_set_error() can be raised from the Route Finder task while
 * dwell_run() blocks - its ERROR status must not be overwritten by IDLE. */
static void run_dwell(size_t node) {
    s_state = STATE_DWELL;
    publish_state(node, POS_NEXT_NONE, ADAM_STATUS_DWELL, false);
    dwell_run();
    publish_trash();

    if (s_state != STATE_DWELL) {
        ESP_LOGW(TAG, "Dwell ended with state %d already taken over, not settling to IDLE", (int)s_state);
        return;
    }
    s_state = STATE_IDLE;
    publish_state(node, POS_NEXT_NONE, ADAM_STATUS_IDLE, false);
}

// [req_id:2 LE][code:1], QoS 1, no retain (06:348). The id comes from the
// caller so it can never be stale; 0 for an error that belongs to no request
// (07:76). Single writer for adam/srv/error.
static void publish_error(uint16_t req_id, adam_error_t reason) {
    adam_req_t data = {
        .id = req_id,
        .code = reason,
    };
    mqtt_publish(MQTT_ERROR_TOPIC, (const char*)&data, 3, 1, false);
}

void executor_set_error(uint16_t req_id, adam_error_t reason) {
    if (s_state == STATE_ERROR) {
        // Already stopped: the state transition must not repeat, but 07:84 still
        // requires the failure to reach the gateway - a reset that finds no route
        // back to Home reports error(req_id=0, NO_PATH) while already ERROR.
        publish_error(req_id, reason);
        ESP_LOGE(TAG, "Already in ERROR: %s reported without re-entering (07:84)", err_name(reason));
        return;
    }
    s_state = STATE_ERROR;

    // Spec 7.3.1: the motor stops and the lid is closed
    motor_halt();
    dwell_close_lid();
    // Spec 7.2: flush every queue
    path_resolver_flush_paths();
    mqtt_flush_jobs();

    publish_error(req_id, reason);
    // Update current_pos and status in one snapshot transaction (07:76).
    publish_state(path_resolver_current(), POS_NEXT_NONE, ADAM_STATUS_ERROR, false);
    ESP_LOGE(TAG, "ERROR: %s, robot stopped and waits for an admin reset (spec 7.2)", err_name(reason));
}

// Raised by the MQTT task when an admin reset arrives mid-ride (07 §7.3.2).
// Only a flag: the executor loop is the sole writer of s_nav/s_state, so a
// cross-task request can never race the navigation state.
static volatile bool s_reset_requested = false;

void executor_cancel_active_path(void) {
    s_reset_requested = true;
}

// Turn onto the edge direction and reset the deadline (spec 3.2). Returns false
// when the edge is not valid in the current graph.
static bool move_begin_step(void) {
    size_t from = s_nav.path[s_step - 1];
    size_t to = s_nav.path[s_step];

    const uint8_t* edges;
    if (!graph_get(&edges)) {
        return false;
    }

    uint8_t edge = edges[(from * GRAPH_NODE_COUNT_MAX) + to];
    if (EDGE_WEIGHT(edge) == NO_EDGE) {
        return false;
    }

    motor_turn_to_heading(EDGE_DIR(edge));
    s_state = STATE_MOVING;

    // Edge start enters MOVING; update status and current_pos atomically (07:58).
    publish_state(from, (int)to, ADAM_STATUS_MOVING, false);

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

    // Home (Node A): face north again (spec 3.1 and 7.2)
    if (to == 0) {
        motor_turn_to_heading(DIR_NORTH);
    }

    if (s_step >= s_nav.len) {
        // Path finished; publish the destination with its settled status directly.
        s_junction_run = 0;

        if (s_nav.job.reset) {
            // Admin recovery skips dwell and settles directly to IDLE.
            publish_state(to, POS_NEXT_NONE, ADAM_STATUS_IDLE, false);
            s_state = STATE_IDLE;
            ESP_LOGI(TAG, "Recovery navigation finished, back to IDLE");
        } else {
            // Dwell on the target node (spec 4) after a normal ride.
            ESP_LOGI(TAG, "Normal navigation finished, dwell on %c", (int)(to + 'A'));
            run_dwell(to);
        }
        return;
    }

    // Intermediate arrival remains MOVING while pointing to the next node.
    publish_state(to, (int)s_nav.path[s_step], ADAM_STATUS_MOVING, false);

    // Start the next edge; move_begin_step publishes its [from][to] snapshot.
    // A failed edge lookup transitions to ERROR directly.
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
        if (s_nav.job.reset) {
            // 07:83 - a reset that lands on Home already only turns the heading
            // to north and returns to IDLE; opening the lid here would run a
            // dwell nobody asked for.
            motor_turn_to_heading(DIR_NORTH);
            publish_state(path_resolver_current(), POS_NEXT_NONE, ADAM_STATUS_IDLE, false);
            s_state = STATE_IDLE;
            ESP_LOGW(TAG, "Reset while idle on Home: heading fixed to north, back to IDLE");
            return;
        }
        // Target = current position: dwell right away (spec 4), without moving
        ESP_LOGI(TAG, "Already on the target node, dwell right away");
        run_dwell(s_nav.path[0]);
        return;
    }

    if (!move_begin_step()) {
        executor_set_error(nav.job.req.id, ADAM_ERR_NO_PATH);
    }
}

static void executor(void* pvParameters) {
    ESP_LOGI(TAG, "Task Executor ON: PID 100 Hz (Kp=%d, Kd=%d), core 1, prio 3", (int)PID_KP, (int)PID_KD);

    // No status before Task Fetch settled (07:38-39): IDLE only with a real
    // graph, ERROR when all 3 attempts failed
    while (!boot_status_settle()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    for (;;) {
        // 07 §7.3.2 - an admin reset abandons the path being executed so the
        // recovery route to Home is not stuck behind it. Consumed here, at a
        // point where the executor owns the navigation state.
        if (s_reset_requested) {
            s_reset_requested = false;
            if (s_state == STATE_MOVING) {
                motor_halt();
                s_step = 0;
                s_nav.len = 0;
                // a half-counted junction must not survive into the next ride
                s_junction_run = 0;
                s_junction_valid = false;
                s_state = STATE_IDLE;
                ESP_LOGW(TAG, "Reset: active path cancelled while leaving %c", (int)(path_resolver_current() + 'A'));
            }
        }

        switch (s_state) {
        case STATE_MOVING:
            moving_tick();
            vTaskDelay(pdMS_TO_TICKS(PID_LOOP_MS));
            break;

        case STATE_IDLE:
            idle_tick();
            break;

        case STATE_DWELL:
            // Unreachable while dwell_run() blocks the task itself; kept so a
            // state handed over from another task cannot fall into `default`
            // and spin without yielding.
            motor_halt();
            vTaskDelay(pdMS_TO_TICKS(50));
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
                        publish_state(path_resolver_current(), POS_NEXT_NONE, ADAM_STATUS_IDLE, false);
                        s_state = STATE_IDLE;
                        ESP_LOGI(TAG, "Recovery finished (already on Home)");
                        break;
                    }

                    s_step = 1;
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
    s_telemetry_publish_mutex = xSemaphoreCreateMutex();
    if (s_telemetry_publish_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(executor, "Task Executor", 4096, NULL, 3, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
