#ifndef SRC_GRAPH_FETCH_H
#define SRC_GRAPH_FETCH_H

#include "esp_err.h"
#include <stdbool.h>

#ifndef GRAPH_SERVER_URL
#    define GRAPH_SERVER_URL "https://adam-three-sand.vercel.app/graph.bin"
#endif

esp_err_t graph_fetch_init(void);

// Returns true when all 3 boot attempts failed or WiFi never connected.
// The executor queries this during boot to decide ERROR vs IDLE status.
bool graph_fetch_has_failed(void);

#endif // SRC_GRAPH_FETCH_H
