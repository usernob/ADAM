#ifndef SRC_GRAPH_H
#define SRC_GRAPH_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIR_NORTH 0b00
#define DIR_EAST 0b01
#define DIR_SOUTH 0b10
#define DIR_WEST 0b11

#define EDGE_ENCODE(weight, dir) ((((uint8_t)(weight) & 0x3F) << 2) | ((dir) & 0x03))

#define EDGE_WEIGHT(edge) (((edge) >> 2) & 0x3F)
#define EDGE_DIR(edge) ((edge) & 0x03)

#define MAX_EDGE_WEIGHT 0x3F

#define NO_EDGE 0x00

typedef uint16_t dist_t;
#define DIST_INF UINT16_MAX

extern const char* STR_DIR[4];

// Because the alphabet only has 26 character
#define GRAPH_NODE_COUNT_MAX 26
#define GRAPH_SIZE (GRAPH_NODE_COUNT_MAX * GRAPH_NODE_COUNT_MAX)

// clang-format off
// dummy graph
extern const uint8_t dummy_graph[GRAPH_SIZE];
// clang-format on

esp_err_t graph_update(const uint8_t* data, uint32_t version);

// Get the active graph matrix (weight + direction), false if no graph set yet
bool graph_get(const uint8_t** edges);

// Installed matrix version, 0 = no ready graph yet (adam/srv/graph_version)
uint32_t graph_version(void);

/* Drop the installed matrix back to "not ready" (version 0). Used when an
 * admin reset fails to re-fetch the graph, so the local gates (graph_get,
 * graph_ready) see the same state the robot reports on adam/srv/graph_version
 * (07 §7.3.2). */
void graph_invalidate(void);

// Matrix installed under version != 0: gate for accepting jobs (07:44) and for
// the gateway to dispatch
bool graph_ready(void);

#endif // SRC_GRAPH_H
