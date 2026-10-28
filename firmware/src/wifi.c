#include "wifi.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "portmacro.h"
#include <stddef.h>
#include <stdint.h>

#define WIFI_STA_MAXIMUM_RETRY 3
#define WIFI_RECONNECT_DELAY_US (10 * 1000 * 1000)

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t* s_wifi_netif;

/* One-shot timer to delay the reconnect after WIFI_STA_MAXIMUM_RETRY failed */
static esp_timer_handle_t s_reconnect_timer;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char* TAG = "wifi station";

static int s_retry_num = 0;

static void reconnect_timer_cb(void* arg) {
    ESP_LOGI(TAG, "auto reconnect: trying to connect again");
    s_retry_num = 0;
    esp_wifi_connect();
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_retry_num < WIFI_STA_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            esp_timer_start_once(s_reconnect_timer, WIFI_RECONNECT_DELAY_US);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        esp_timer_stop(s_reconnect_timer);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "station lost its IP address");
    }
}

esp_err_t wifi_init_sta() {
    ESP_LOGI(TAG, "Starting WiFi in STA mode...");
    s_wifi_event_group = xEventGroupCreate();

    esp_timer_create_args_t timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "Failed to create reconnect timer");

    s_wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Failed to initialize wifi");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_lost_ip;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id), TAG,
        "Failed to register wifi handler for wifi event");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip), TAG,
        "Failed to register wifi handler for ip event");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &event_handler, NULL, &instance_lost_ip),
        TAG, "Failed to register wifi handler for lost ip event");

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    wifi_config_t config = {
        .sta =
            {
                .ssid = WIFI_SSID,
                .password = WIFI_PASS,
            },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "Failed to set wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start wifi");

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to ap SSID: %s", config.sta.ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Failed to connect to SSID: %s", config.sta.ssid);
        ESP_LOGW(TAG, "Automatic Reconnect every %d seconds", WIFI_RECONNECT_DELAY_US / 1000000);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_ERR_INVALID_STATE;
    }
}

bool wifi_wait_connected(uint32_t timeout_ms) {
    if (s_wifi_event_group == NULL) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return false;
        }

        EventBits_t bits =
            xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, timeout - elapsed);
        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            return false;
        }

        // A disconnect or lost-IP event may race with a previously observed bit.
        // Require both an active AP association and a nonzero netif IPv4 address.
        wifi_ap_record_t ap_info;
        esp_netif_ip_info_t ip_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && s_wifi_netif != NULL &&
            esp_netif_get_ip_info(s_wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            return true;
        }

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
