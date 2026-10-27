#ifndef SRC_WIFI_H
#define SRC_WIFI_H

#include "esp_err.h"

// WiFi parameters
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

esp_err_t wifi_init_sta();

/* Wait until the STA has an IP (WIFI_CONNECTED_BIT). Safe to call again
 * while already connected, it returns right away. */
void wifi_wait_connected(void);

#endif // SRC_WIFI_H
