#include "dwell.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/ledc_types.h"
#include "sensors.h"
#include "soc/clk_tree_defs.h"
#include <stdbool.h>
#include <stdint.h>

static const char* TAG = "dwell";

// Trash lid servo (calibration: 500-2400 us)
#define DWELL_SERVO_PIN 23
#define DWELL_SERVO_FREQ_HZ 50
#define DWELL_SERVO_RESOLUTION 14
#define DWELL_SERVO_MIN_US 500
#define DWELL_SERVO_MAX_US 2400
#define DWELL_SERVO_RANGE_DEG 180
#define DWELL_SERVO_LEDC_TIMER LEDC_TIMER_1
#define DWELL_SERVO_LEDC_CHANNEL LEDC_CHANNEL_2

// Sampling period of the hand IR sensor during the dwell
#define DWELL_SAMPLE_MS 200

// Monotonic time in milliseconds
static inline int64_t dwell_now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// Write a servo angle from 0 to 180 degrees (pulse width 500-2400 us)
static void servo_write_deg(int deg) {
    if (deg < 0) {
        deg = 0;
    }
    if (deg > DWELL_SERVO_RANGE_DEG) {
        deg = DWELL_SERVO_RANGE_DEG;
    }

    uint32_t us =
        DWELL_SERVO_MIN_US + ((uint32_t)((DWELL_SERVO_MAX_US - DWELL_SERVO_MIN_US) * deg) / DWELL_SERVO_RANGE_DEG);
    uint32_t duty = (uint32_t)(((uint64_t)us * (1U << DWELL_SERVO_RESOLUTION)) / 20000U);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, DWELL_SERVO_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, DWELL_SERVO_LEDC_CHANNEL);
}

esp_err_t dwell_init(void) {
    // Dedicated LEDC timer for the 50 Hz servo
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = DWELL_SERVO_LEDC_TIMER,
        .duty_resolution = DWELL_SERVO_RESOLUTION,
        .freq_hz = DWELL_SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "servo ledc timer failed");

    const ledc_channel_config_t chan = {
        .gpio_num = DWELL_SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = DWELL_SERVO_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = DWELL_SERVO_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chan), TAG, "servo ledc channel failed");

    servo_write_deg(DWELL_SERVO_CLOSE_DEG);
    ESP_LOGI(TAG, "Lid servo ready (pin %d, 50 Hz, %d-%d us)", DWELL_SERVO_PIN, DWELL_SERVO_MIN_US, DWELL_SERVO_MAX_US);
    return ESP_OK;
}

void dwell_close_lid(void) {
    servo_write_deg(DWELL_SERVO_CLOSE_DEG);
}

void dwell_run(void) {
    ESP_LOGI(TAG, "Reached the target node, opening the lid (servo %d deg)", DWELL_SERVO_OPEN_DEG);
    servo_write_deg(DWELL_SERVO_OPEN_DEG);

    // Arrival notification: 2 short beeps
    sensors_buzzer_beep(80);
    vTaskDelay(pdMS_TO_TICKS(50));
    sensors_buzzer_beep(80);

    int64_t dwell_start = dwell_now_ms();
    int64_t total_start = dwell_start;
    bool hand_was_present = false;
    bool capped = false;

    // Dwell loop: 15 second timer, reset while the hand IR sees a hand (spec 4).
    // DWELL_TOTAL_MAX_MS caps the entire stop, so a hand that never leaves the
    // opening cannot hold the node forever (spec 4: robot like an elevator).
    while (dwell_now_ms() - dwell_start < DWELL_TIME_MS) {
        if (dwell_now_ms() - total_start >= DWELL_TOTAL_MAX_MS) {
            capped = true;
            break;
        }
        bool hand_present = sensors_hand_detected();

        if (hand_present) {
            dwell_start = dwell_now_ms();
            if (!hand_was_present) {
                ESP_LOGI(TAG, "Hand detected at the trash opening, dwell timer extended");
            }
        } else if (hand_was_present) {
            ESP_LOGI(TAG, "Hand removed, dwell timer continues");
        }
        hand_was_present = hand_present;

        vTaskDelay(pdMS_TO_TICKS(DWELL_SAMPLE_MS));
    }

    if (capped) {
        ESP_LOGW(TAG, "Dwell total cap (%d ms) reached, closing the lid (servo %d deg)", DWELL_TOTAL_MAX_MS,
                 DWELL_SERVO_CLOSE_DEG);
    } else {
        ESP_LOGI(TAG, "Dwell timer expired, closing the lid (servo %d deg)", DWELL_SERVO_CLOSE_DEG);
    }
    servo_write_deg(DWELL_SERVO_CLOSE_DEG);

    // Dwell finished notification. The caller (Task Executor) republishes the
    // trash capacity right after this returns, once the lid is closed again.
    sensors_buzzer_beep(120);
}
