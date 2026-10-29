#ifndef SRC_WIFI_H
#define SRC_WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t wifi_init_sta();

/* Wait up to timeout_ms for the STA to obtain an IP. */
bool wifi_wait_connected(uint32_t timeout_ms);

#endif // SRC_WIFI_H
