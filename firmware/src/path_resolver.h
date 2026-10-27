#ifndef SRC_PATH_RESOLVER_H
#define SRC_PATH_RESOLVER_H

#include "esp_err.h"
#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>

// Max nodes per navigation path: a graph holds at most 26 labels 'A'..'Z'
// (docs/02-graph.md 2.1); a bigger graph is rejected by the Route Finder.
#define NAV_MAX_NODES 26

// One path ready to execute: node indexes A.., its length and the job behind it
// (an admin reset is also accepted while the status is ERROR)
typedef struct {
    adam_job_t job;
    size_t path[NAV_MAX_NODES];
    size_t len;
} nav_path_t;

esp_err_t path_resolver_init(void);

// Non blocking queue fetching (Task Executor)
bool path_resolver_get_nav(nav_path_t* out);

// The Task Executor reports the real arrival on a node (the actual position
// used for the next Dijkstra calculation - spec 7.2)
void path_resolver_set_current(size_t node);

// Current confirmed node (0 = 'A')
size_t path_resolver_current(void);

// Flush the path queue (called by the Task Executor on LINE_LOST, spec 7.2)
void path_resolver_flush_paths(void);

#endif // SRC_PATH_RESOLVER_H
