#include "sensors.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "hal/gpio_types.h"
#include "soc/gpio_num.h"
#include <stddef.h>
#include <stdint.h>

static const char* TAG = "sensors";

/* TCS3200 sensing rule: a bright marker gives a short pulse (LOW < 150 us
 * with the 20% scaling) while a black line gives a long pulse. */
#define TCS_BRIGHT_PULSE_US 150
// Timeout per measurement, the sensor needs at most 20 ms per channel
#define TCS_PULSE_TIMEOUT_US 20000
// Maximum HC-SR04 distance, also used as the timeout value (cm)
#define SENSORS_US_TIMEOUT_CM 400.0f
// Minimum readable HC-SR04 distance (cm)
#define SENSORS_US_MIN_CM 2.0f
// Speed of sound 0.0343 cm/us (distance is halved, there and back)
#define SENSORS_US_CM_PER_US 0.0343f
// ADC1_CH0 (GPIO36) battery divider scale, 0-4095
#define SENSORS_ADC_MAX 4095

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
// Polls every 1 us with esp_timer (called rarely, only on a junction)
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
#if SENSORS_LINE_ACTIVE_LOW
        line[i] = (level == 0) ? 1 : 0;
#else
        line[i] = (level != 0) ? 1 : 0;
#endif
    }
}

// Detect a color marker on a node (spec 2.2). Red = S2 low/S3 low,
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
