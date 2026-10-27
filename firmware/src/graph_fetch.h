#ifndef SRC_GRAPH_FETCH_H
#define SRC_GRAPH_FETCH_H

#include "esp_err.h"

#ifndef GRAPH_SERVER_URL
#    define GRAPH_SERVER_URL "https://adam-three-sand.vercel.app/graph.bin"
#endif

esp_err_t graph_fetch_init(void);

#endif // SRC_GRAPH_FETCH_H
