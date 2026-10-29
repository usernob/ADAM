#ifndef SRC_GRAPH_FETCH_H
#define SRC_GRAPH_FETCH_H

#include "esp_err.h"
#include <stdbool.h>

/* Start the one-shot Task Fetch at boot (07 §7.1). Returns once the task is
 * created, not once the fetch settled - use graph_fetch_success() for that. */
esp_err_t graph_fetch_init(void);

/* Admin reset (07 §7.3.2): run the fetch again in a fresh Task Fetch and block
 * until it settled. The fetch needs ~8 KB for the TLS handshake, so it never
 * runs on the caller's stack. Returns true when a graph was installed. */
bool graph_fetch_rerun(void);

/* True when the last fetch installed a graph. Blocks until the fetch has
 * settled (success, WiFi timeout or all 3 attempts), so a caller must not
 * hold a lock it needs on the fetch path. */
bool graph_fetch_success(void);

#endif // SRC_GRAPH_FETCH_H
