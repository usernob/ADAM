#ifndef SRC_GRAPH_FETCH_H
#define SRC_GRAPH_FETCH_H

#include "esp_err.h"
#include <stdbool.h>

#ifndef GRAPH_SERVER_URL
#    define GRAPH_SERVER_URL "https://adam-three-sand.vercel.app/graph.bin"
#endif

esp_err_t graph_fetch_init(void);

/* Run the full fetch sequence now: wait for a WiFi IP (bounded), then up to
 * 3 HTTP attempts with 2 s gaps. Returns true when a graph was installed.
 * Used at boot by Task Fetch and on admin reset (07 §7.3.2). */
bool graph_fetch_run(void);

// Returns true when the last graph_fetch_run() failed (WiFi timeout or all
// 3 attempts). The executor queries this to decide ERROR vs IDLE status.
bool graph_fetch_has_failed(void);

#endif // SRC_GRAPH_FETCH_H
