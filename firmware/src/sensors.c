#include "sensors.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "hal/gpio_types.h"
#include "portmacro.h"
#include "soc/gpio_num.h"
#include <stddef.h>
#include <stdint.h>

static const char* TAG = "sensors";

/* Mock is selected by the build flag -DADAM_SENSOR_MOCK (platformio.ini:
 * env debug / debug-sensor-mock). The default release build reads the real
 * sensors. */
#ifndef ADAM_SENSOR_MOCK

/* TCS3200 sensing rule: a bright marker gives a short pulse (LOW < 150 us
 * with the 20% scaling) while a black line gives a long pulse. */
#    define TCS_BRIGHT_PULSE_US 150
// Timeout per measurement, the sensor needs at most 20 ms per channel
#    define TCS_PULSE_TIMEOUT_US 20000
// Maximum HC-SR04 distance, also used as the timeout value (cm)
#    define SENSORS_US_TIMEOUT_CM 400.0f
// Minimum readable HC-SR04 distance (cm)
#    define SENSORS_US_MIN_CM 2.0f
// Speed of sound 0.0343 cm/us (distance is halved, there and back)
#    define SENSORS_US_CM_PER_US 0.0343f
// ADC1_CH0 (GPIO36) battery divider scale, 0-4095
#    define SENSORS_ADC_MAX 4095

static adc_oneshot_unit_handle_t s_adc = NULL;

// Select the TCS3200 color filter (S2/S3) before measuring
static esp_err_t tcs_write(int s2, int s3) {
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)SENSORS_PIN_TCS_S2, s2), TAG, "S2 write failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)SENSORS_PIN_TCS_S3, s3), TAG, "S3 write failed");
    // Let the internal TCS3200 filter settle before the measurement gate
    esp_rom_delay_us(500);
    return ESP_OK;
}

// Low pulse width (us) on TCS_OUT, similar to pulseIn(pin, LOW, timeout)
// Polls every 1 us with esp_timer (called periodically by the executor to
// sample the node color marker)
static uint32_t tcs_measure_pulse_low_us(void) {
    const int64_t deadline = TCS_PULSE_TIMEOUT_US;
    int64_t start;

    // Wait for the pin to go HIGH first so the measurement starts on the falling edge
    start = esp_timer_get_time();
    while (!gpio_get_level((gpio_num_t)SENSORS_PIN_TCS_OUT)) {
        if (esp_timer_get_time() - start > deadline) {
            return deadline; // no pulse completed
        }
    }
    // Wait for the falling edge (start of the LOW pulse)
    start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)SENSORS_PIN_TCS_OUT)) {
        if (esp_timer_get_time() - start > deadline) {
            return deadline;
        }
    }
    start = esp_timer_get_time();
    // Wait for the rising edge (end of the LOW pulse)
    while (!gpio_get_level((gpio_num_t)SENSORS_PIN_TCS_OUT)) {
        if (esp_timer_get_time() - start > deadline) {
            return deadline; // long pulse = dark surface
        }
    }
    return (uint32_t)(esp_timer_get_time() - start);
}

esp_err_t sensors_init(void) {
    // BFD-1000 line channels (left=39, mid=34, right=35) + hand IR, HC-SR04 echo and TCS3200 output as inputs
    const gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << SENSORS_PIN_LF_LEFT) | (1ULL << SENSORS_PIN_LF_MIDDLE) |
                        (1ULL << SENSORS_PIN_LF_RIGHT) | (1ULL << SENSORS_PIN_HAND_IR) | (1ULL << SENSORS_PIN_US_ECHO) |
                        (1ULL << SENSORS_PIN_TCS_OUT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_cfg), TAG, "input gpio failed");

    const gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << SENSORS_PIN_TCS_S0) | (1ULL << SENSORS_PIN_TCS_S1) | (1ULL << SENSORS_PIN_TCS_S2) |
                        (1ULL << SENSORS_PIN_TCS_S3) | (1ULL << SENSORS_PIN_US_TRIG) | (1ULL << SENSORS_PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_cfg), TAG, "output gpio failed");

    // TCS3200 output scaling 20% (S0=HIGH, S1=LOW)
    gpio_set_level((gpio_num_t)SENSORS_PIN_TCS_S0, 1);
    gpio_set_level((gpio_num_t)SENSORS_PIN_TCS_S1, 0);
    ESP_ERROR_CHECK(tcs_write(0, 0));

    // ADC1_CH0 (GPIO36) battery voltage divider
    const adc_oneshot_unit_init_cfg_t adc_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc unit failed");
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan_cfg), TAG, "adc channel failed");

    gpio_set_level((gpio_num_t)SENSORS_PIN_BUZZER, 0);
    gpio_set_level((gpio_num_t)SENSORS_PIN_US_TRIG, 0);

    ESP_LOGI(TAG, "Sensors ready: BFD-1000 5x IR, hand IR, TCS3200, HC-SR04, buzzer");
    return ESP_OK;
}

void sensors_line_read(int line[SENSORS_LINE_COUNT]) {
    // Order: line[0]=left (GPIO39/S_LEFT), line[1]=mid (GPIO34/S_MIDDLE), line[2]=right (GPIO35/S_RIGHT)
    static const gpio_num_t pins[SENSORS_LINE_COUNT] = {
        (gpio_num_t)SENSORS_PIN_LF_LEFT,
        (gpio_num_t)SENSORS_PIN_LF_MIDDLE,
        (gpio_num_t)SENSORS_PIN_LF_RIGHT,
    };

    for (int i = 0; i < SENSORS_LINE_COUNT; i++) {
        int level = gpio_get_level(pins[i]);
#    if SENSORS_LINE_ACTIVE_LOW
        line[i] = (level == 0) ? 1 : 0;
#    else
        line[i] = (level != 0) ? 1 : 0;
#    endif
    }
}

// Detect a color marker on a node (spec 2.3). Red = S2 low/S3 low,
// Green = S2 high/S3 high, Blue = S2 low/S3 high.
bool sensors_color_marker_present(void) {
    uint32_t r;
    uint32_t g;
    uint32_t b;

    if (tcs_write(0, 0) != ESP_OK) {
        return false;
    }
    r = tcs_measure_pulse_low_us();

    if (tcs_write(1, 1) != ESP_OK) {
        return false;
    }
    g = tcs_measure_pulse_low_us();

    if (tcs_write(0, 1) != ESP_OK) {
        return false;
    }
    b = tcs_measure_pulse_low_us();

    ESP_LOGD(TAG, "TCS pulse us r=%lu g=%lu b=%lu", (unsigned long)r, (unsigned long)g, (unsigned long)b);

    // Every channel timed out (pulse longer than 20 ms) = no signal at all
    bool any_signal = (r < TCS_PULSE_TIMEOUT_US) || (g < TCS_PULSE_TIMEOUT_US) || (b < TCS_PULSE_TIMEOUT_US);
    if (!any_signal) {
        return false;
    }

    // Bright marker: one of the channels gives a short pulse (< 150 us)
    return (r < TCS_BRIGHT_PULSE_US) || (g < TCS_BRIGHT_PULSE_US) || (b < TCS_BRIGHT_PULSE_US);
}

float sensors_ultrasonic_distance_cm(void) {
    // 10 us trigger pulse (HC-SR04)
    gpio_set_level((gpio_num_t)SENSORS_PIN_US_TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level((gpio_num_t)SENSORS_PIN_US_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level((gpio_num_t)SENSORS_PIN_US_TRIG, 0);

    // Wait for the echo to rise (at most 10 ms)
    int64_t start = esp_timer_get_time();
    int64_t rise_time = -1;
    while (esp_timer_get_time() - start < 10000) {
        if (gpio_get_level((gpio_num_t)SENSORS_PIN_US_ECHO)) {
            rise_time = esp_timer_get_time();
            break;
        }
        esp_rom_delay_us(10);
    }
    if (rise_time < 0) {
        return SENSORS_US_TIMEOUT_CM;
    }

    // Wait for the echo to fall (at most 25 ms ~ 4 m)
    while (gpio_get_level((gpio_num_t)SENSORS_PIN_US_ECHO)) {
        if (esp_timer_get_time() - rise_time > 25000) {
            return SENSORS_US_TIMEOUT_CM;
        }
    }

    // Speed of sound 0.0343 cm/us, distance halved because of the round trip
    float cm = (float)(esp_timer_get_time() - rise_time) * SENSORS_US_CM_PER_US / 2.0f;
    if (cm < SENSORS_US_MIN_CM) {
        cm = SENSORS_US_MIN_CM;
    }
    if (cm > SENSORS_US_TIMEOUT_CM) {
        cm = SENSORS_US_TIMEOUT_CM;
    }
    return cm;
}

uint8_t sensors_trash_level_percent(void) {
    float cm = sensors_ultrasonic_distance_cm();
    float span = SENSORS_TRASH_EMPTY_CM - SENSORS_TRASH_FULL_CM;
    if (span <= 0.0f) {
        return 0;
    }

    float level = (SENSORS_TRASH_EMPTY_CM - cm) / span * 100.0f;
    if (level < 0.0f) {
        return 0;
    }
    if (level > 100.0f) {
        return 100;
    }
    return (uint8_t)(level + 0.5f);
}

bool sensors_hand_detected(void) {
    // FC-51 hand sensor output: LOW = hand/object detected
    return gpio_get_level((gpio_num_t)SENSORS_PIN_HAND_IR) == 0;
}

uint8_t sensors_battery_percent(void) {
    int raw = 0;
    if (s_adc == NULL || adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
        return 0;
    }
    if (raw < 0) {
        raw = 0;
    }
    if (raw > SENSORS_ADC_MAX) {
        raw = SENSORS_ADC_MAX;
    }
    return (uint8_t)((raw * 100) / SENSORS_ADC_MAX);
}

void sensors_buzzer_beep(int ms) {
    gpio_set_level((gpio_num_t)SENSORS_PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level((gpio_num_t)SENSORS_PIN_BUZZER, 0);
}

#else

/* Sensor mock (ADAM_SENSOR_MOCK): runs the whole firmware without hardware.
 * One task advances a scripted scenario and every sensors_* call reads the
 * resulting state, so the executor, dwell loop and MQTT telemetry all see a
 * consistent world:
 *  - line sensors: driven by the real graph. While the executor is MOVING the
 *    mock looks up the edge being driven in the installed matrix:
 *      - edge missing (weight 0) or no graph -> no line at all, so the
 *        executor raises LINE_LOST like an off-track robot;
 *      - valid edge -> straight line {0,1,0} for weight * MOCK_TICKS_PER_WEIGHT
 *        ticks, then a node window of MOCK_NODE_TICKS ticks (weight is the
 *        graph's relative distance, 1..63).
 *    Inside the node window the pattern mirrors the graph topology at the
 *    arrival node, relative to the incoming heading: centre always lit, plus
 *    the right sensor for a branch at +90 deg and the left sensor for -90 deg
 *    (the incoming edge behind is not drawn). A straight node or a dead end
 *    stays {0,1,0}.
 *    The color marker only lights up inside that same node window - it is the
 *    node marker, so edge intervals report false (spec 2.3).
 *  - hand IR: 5 s "hand present", 5 s away, so the dwell timer gets extended
 *  - trash: +1 % every 5 s from 40 %, reaching the full threshold in ~5 min
 *    (exercises auto-park and the TRASH_FULL/bin_full gates)
 *  - battery: -1 % every 30 s from 100 %
 * Timings/values are macros so a test can be sped up or slowed down. */
#    include "executor.h"
#    include "graph.h"

#    define MOCK_TICK_MS 100
#    define MOCK_TICKS_PER_WEIGHT 4   // 4 x 100 ms per weight unit (63 -> 25.2 s, under the 30 s step timeout)
#    define MOCK_NODE_TICKS 6         // 600 ms node window: enough for 2 marker samples 100 ms apart
#    define MOCK_HAND_PERIOD_TICKS 50 // 5 s hand present, then 5 s away
#    define MOCK_TRASH_START_PERCENT 40
#    define MOCK_TRASH_STEP_TICKS 50 // +1 % every 5 s
#    define MOCK_BATTERY_START_PERCENT 100
#    define MOCK_BATTERY_STEP_TICKS 300 // -1 % every 30 s

typedef struct {
    int line[SENSORS_LINE_COUNT];
    uint8_t battery;
    uint8_t trash_level;
    bool hand_detected;
    bool color_detected;
} sensor_mock_state_t;

static sensor_mock_state_t s_state = {
    .line = {0, 1, 0},
    .battery = MOCK_BATTERY_START_PERCENT,
    .trash_level = MOCK_TRASH_START_PERCENT,
    .hand_detected = false,
    .color_detected = false,
};

static void update_sensor(void* pvParameters) {
    TickType_t last = xTaskGetTickCount();
    uint32_t ticks = 0;
    uint32_t edge_ticks = 0;
    size_t edge_from = (size_t)-1;
    size_t edge_to = (size_t)-1;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MOCK_TICK_MS));

        // The line follows the edge the executor is driving, checked against
        // the installed graph: no edge means no line (real off-track robot)
        size_t from;
        size_t to;
        bool moving = executor_active_edge(&from, &to);

        if (!moving) {
            // At a node or dwelling: keep a line under the robot, unused then
            s_state.color_detected = false;
            s_state.line[0] = 0;
            s_state.line[1] = 1;
            s_state.line[2] = 0;
            edge_from = (size_t)-1;
            edge_to = (size_t)-1;
            edge_ticks = 0;
        } else {
            if (from != edge_from || to != edge_to) {
                // New edge: restart the travel clock
                edge_from = from;
                edge_to = to;
                edge_ticks = 0;
            }

            const uint8_t* edges = NULL;
            uint8_t weight = 0;
            if (graph_get(&edges)) {
                weight = EDGE_WEIGHT(edges[(from * GRAPH_NODE_COUNT_MAX) + to]);
            }

            if (weight == NO_EDGE) {
                s_state.color_detected = false;
                s_state.line[0] = 0;
                s_state.line[1] = 0;
                s_state.line[2] = 0;
                if (edge_ticks == 0) {
                    ESP_LOGW(TAG, "mock: no edge %c -> %c in the graph, line lost", (int)(from + 'A'), (int)(to + 'A'));
                }
            } else {
                uint32_t travel = (uint32_t)weight * MOCK_TICKS_PER_WEIGHT;
                // Keep at least one non-node tick at the start of every edge:
                // the executor's anti false-arrival latch needs to see the
                // departure node's marker disappear before it accepts the next
                // one. Without this, a weight-1 edge (travel 4 ticks) would be
                // a node window from its very first tick.
                uint32_t node_ticks = MOCK_NODE_TICKS;
                if (node_ticks >= travel) {
                    node_ticks = (travel > 1) ? travel - 1 : 1;
                }
                bool node_window = (edge_ticks + node_ticks) >= travel;

                // Edge interval: only the centre sensor sees the line. Node
                // interval (last ticks): the pattern mirrors the graph at the
                // arrival node and the color marker lights up in exactly the
                // same window (spec 2.3). Write the marker first: the executor
                // samples it on the first tick of the node window, so it must
                // never see a branch pattern before the color flag is set.
                s_state.color_detected = node_window;
                s_state.line[0] = 0;
                s_state.line[1] = 1;
                s_state.line[2] = 0;

                if (node_window) {
                    // Centre is always lit at a node. Neighbours are mapped
                    // relative to the incoming heading: +1 = right branch,
                    // +3 = left branch, 0 = straight continuation (centre
                    // only), 2 = the incoming edge behind (not drawn).
                    int incoming = EDGE_DIR(edges[(from * GRAPH_NODE_COUNT_MAX) + to]);
                    for (size_t n = 0; n < GRAPH_NODE_COUNT_MAX; n++) {
                        uint8_t e = edges[(to * GRAPH_NODE_COUNT_MAX) + n];
                        if (EDGE_WEIGHT(e) == NO_EDGE) {
                            continue;
                        }
                        int delta = (EDGE_DIR(e) - incoming + 4) % 4;
                        if (delta == 1) {
                            s_state.line[2] = 1; // right branch
                        } else if (delta == 3) {
                            s_state.line[0] = 1; // left branch
                        }
                    }
                }
            }
            edge_ticks++;
        }

        // Hand at the trash opening: half of the period present, half away
        s_state.hand_detected = (ticks % (MOCK_HAND_PERIOD_TICKS * 2)) < MOCK_HAND_PERIOD_TICKS;

        if (ticks > 0 && (ticks % MOCK_TRASH_STEP_TICKS) == 0 && s_state.trash_level < 100) {
            s_state.trash_level++;
        }
        if (ticks > 0 && (ticks % MOCK_BATTERY_STEP_TICKS) == 0 && s_state.battery > 0) {
            s_state.battery--;
        }
        ticks++;
    }
}

esp_err_t sensors_init(void) {
    ESP_RETURN_ON_FALSE(xTaskCreate(update_sensor, "Update Sensor mock", 2048, NULL, 1, NULL) == pdTRUE, ESP_ERR_NO_MEM,
                        TAG, "Failed to create sensor mock task");
    ESP_LOGW(TAG, "SENSOR MOCK ACTIVE: line/node marker, hand IR, trash and battery are simulated");
    return ESP_OK;
}

void sensors_line_read(int line[SENSORS_LINE_COUNT]) {
    for (int i = 0; i < SENSORS_LINE_COUNT; i++) {
        line[i] = s_state.line[i];
    }
}

bool sensors_color_marker_present(void) {
    return s_state.color_detected;
}

float sensors_ultrasonic_distance_cm(void) {
    // Mirror the real formula so distance and trash level always agree
    float span = SENSORS_TRASH_EMPTY_CM - SENSORS_TRASH_FULL_CM;
    return SENSORS_TRASH_EMPTY_CM - ((float)s_state.trash_level / 100.0f) * span;
}

uint8_t sensors_trash_level_percent(void) {
    return s_state.trash_level;
}

bool sensors_hand_detected(void) {
    return s_state.hand_detected;
}

uint8_t sensors_battery_percent(void) {
    return s_state.battery;
}

void sensors_buzzer_beep(int ms) {
    ESP_LOGI(TAG, "buzzer beep %d ms (mock)", ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

#endif /* ifndef ADAM_SENSOR_MOCK */
