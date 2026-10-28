#include "dwell.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "executor.h"
#include "freertos/projdefs.h"
#include "graph_fetch.h"
#include "motor.h"
#include "mqtt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "path_resolver.h"
#include "sensors.h"
#include "wifi.h"
#include <inttypes.h>

static const char* TAG = "main";

#ifndef ADAM_MOTOR_DEMO
// Normal mode: the full application (MQTT, Dijkstra, sensors, dwell)

// Main application
void app_main() {
    ESP_LOGI(TAG, "Startup..");
    ESP_LOGI(TAG, "Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the global network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Hardware layer first (motor, sensors, servo) - spec 7.1 Task Executor
    ESP_ERROR_CHECK(motor_init());
    ESP_ERROR_CHECK(sensors_init());
    ESP_ERROR_CHECK(dwell_init());

    // Boot on Node 'A' facing north, status IDLE (spec 3.1)
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_init_sta());

    // Consumers before the producer: the MQTT connect handler publishes the
    // neutral boot snapshot (graph_version, [A][0], IDLE) before online = 1
    // (spec 06 §6.4), so the executor must be initialized before the client
    // can connect. The Route Finder simply waits for the mailbox.
    ESP_ERROR_CHECK(path_resolver_init());
    ESP_ERROR_CHECK(executor_init());

    // The graph is downloaded from the server (GET /graph.bin, https) - Task Fetch
    ESP_ERROR_CHECK(graph_fetch_init());

    ESP_ERROR_CHECK(mqtt_app_start());
}

#else
// Motor demo mode: L298N only, no WiFi/MQTT/sensors/navigation
// Sequence: forward -> coast -> backward -> coast -> left -> right -> stop
#    include "freertos/FreeRTOS.h"
#    include "freertos/task.h"

void app_main() {
    ESP_LOGI(TAG, "=== ADAM MOTOR DEMO ===");
    motor_init();

    const int speed = 160; // target PWM for line following (PID base speed)
    for (;;) {
        ESP_LOGI(TAG, "[1] FORWARD %d (2000 ms)", speed);
        motor_set_speed(speed, speed);
        vTaskDelay(pdMS_TO_TICKS(2000));

        ESP_LOGI(TAG, "[2] COAST 1000 ms");
        motor_set_speed(0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "[3] BACKWARD %d (1500 ms)", speed);
        motor_set_speed(-speed, -speed);
        vTaskDelay(pdMS_TO_TICKS(1500));

        ESP_LOGI(TAG, "[4] COAST 1000 ms");
        motor_set_speed(0, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "[5] TURN LEFT (1000 ms)");
        motor_set_speed(-120, 120);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "[6] COAST 500 ms");
        motor_set_speed(0, 0);
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, "[7] TURN RIGHT (1000 ms)");
        motor_set_speed(120, -120);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(TAG, "[8] STOP 5000 ms");
        motor_halt();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif
