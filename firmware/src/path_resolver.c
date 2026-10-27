#include "path_resolver.h"

#include "esp_err.h"
#include "esp_log.h"
#include "executor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "graph.h"
#include "mqtt.h"
#include "portmacro.h"
#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "path_resolver";

static QueueHandle_t s_path_queue;
static volatile size_t s_current_node = 0; // Node 'A' = 0, boot facing north (spec 3.1)

static int find_min_distance(const dist_t dist[restrict GRAPH_NODE_COUNT_MAX],
                             const bool visited[restrict GRAPH_NODE_COUNT_MAX]) {
    dist_t min_distance = DIST_INF;
    int min_node = -1;

    for (size_t i = 0; i < GRAPH_NODE_COUNT_MAX; i++) {
        if (!visited[i] && dist[i] < min_distance) {
            min_distance = dist[i];
            min_node = i;
        }
    }

    return min_node;
}

static bool dijkstra(const uint8_t graph[restrict GRAPH_SIZE], size_t source, size_t target,
                     dist_t dist[restrict GRAPH_NODE_COUNT_MAX], size_t out[restrict GRAPH_NODE_COUNT_MAX]) {
    if (source >= GRAPH_NODE_COUNT_MAX || target >= GRAPH_NODE_COUNT_MAX) {
        return false;
    }

    // Zero-initialized: an uninit array here makes find_min_distance()
    // read stack garbage and the route non-deterministic
    bool visited[GRAPH_NODE_COUNT_MAX] = {0};

    for (size_t i = 0; i < GRAPH_NODE_COUNT_MAX; i++) {
        dist[i] = DIST_INF;
        out[i] = (size_t)-1;
    }

    dist[source] = 0;

    for (size_t step = 0; step < GRAPH_NODE_COUNT_MAX; step++) {
        int u = find_min_distance(dist, visited);

        if (u == -1) {
            break;
        }

        visited[u] = true;

        if ((size_t)u == target) {
            break;
        }

        for (size_t v = 0; v < GRAPH_NODE_COUNT_MAX; v++) {
            uint8_t edge = graph[((size_t)u * GRAPH_NODE_COUNT_MAX) + v];

            if (EDGE_WEIGHT(edge) == NO_EDGE || visited[v]) {
                continue;
            }

            uint8_t weight = EDGE_WEIGHT(edge);

            if (dist[u] > DIST_INF - weight) {
                continue;
            }

            dist_t new_distance = dist[u] + weight;

            if (new_distance < dist[v]) {
                dist[v] = new_distance;
                out[v] = (size_t)u;
            }
        }
    }

    return dist[target] != DIST_INF;
}

static int build_path(size_t target, const size_t* previous, size_t* out, int max) {
    int n = 0;

    while (target != (size_t)-1 && n < max) {
        out[n++] = target;
        target = previous[target];
    }

    for (int i = 0; i < n / 2; i++) {
        size_t t = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = t;
    }

    return n;
}

static inline char itochar(size_t i) {
    return i + 'A';
}

static inline size_t chartoi(char c) {
    return c - 'A';
}

// Wrap the Dijkstra result into a nav_path_t and queue it for the Task Executor
static void enqueue_nav(size_t* order, int n, const adam_job_t* job, const char* target) {
    nav_path_t nav = {
        .len = (size_t)n,
    };
    memcpy(nav.path, order, (size_t)n * sizeof(size_t));
    memcpy(&nav.job, job, sizeof(adam_job_t));

    // Worst case "A -> B -> ..." for 26 nodes = 101 chars + NUL
    char route[(GRAPH_NODE_COUNT_MAX * 4) + 1];
    int off = 0;
    for (int i = 0; i < n && off < (int)sizeof(route) - 5; i++) {
        off += snprintf(route + off, sizeof(route) - off, i ? " -> %c" : "%c", itochar(order[i]));
    }
    ESP_LOGI(TAG, "Path '%s': %s (recovery=%d)", target, route, nav.job.reset);

    // Queue full means queue flooding, drop the oldest item
    if (xQueueSend(s_path_queue, &nav, (TickType_t)0) != pdTRUE) {
        nav_path_t stale;
        xQueueReceive(s_path_queue, &stale, 0);
        xQueueSend(s_path_queue, &nav, 0);
        ESP_LOGW(TAG, "Path queue full, dropping the oldest path");
    }
}

static void path_resolver(void* pvParameters) {
    for (;;) {
        const uint8_t* edges = NULL;
        if (!graph_get(&edges)) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        // Wait for a command from the server (move or admin reset)
        adam_job_t job;
        if (!mqtt_get_job_blocked(&job)) {
            continue;
        }

        size_t dest_node;
        if (job.reset) {
            // Spec 6.2/7.2: admin reset returns to the Home Node 'A' from the
            // confirmed position, the heading is handled by the Task Executor
            dest_node = 0;
            ESP_LOGW(TAG, "Admin RESET command: recovery route %c -> %c", itochar(s_current_node), itochar(dest_node));
        } else {
            size_t target = chartoi(job.req.node);
            if (target >= GRAPH_NODE_COUNT_MAX) {
                ESP_LOGW(TAG, "Target node '%c' is outside the graph", job.req.node);
                continue;
            }
            dest_node = target;
            if (dest_node == s_current_node) {
                // Already on the target: queue a 1-node path, the Task Executor
                // runs the dwell on that node right away (spec 4)
                size_t order[1] = {dest_node};
                enqueue_nav(order, 1, &job, "dwell in place");
                continue;
            }
        }

        ESP_LOGI(TAG, "Calculate route from %c to %c", itochar(s_current_node), itochar(dest_node));

        dist_t dist[GRAPH_NODE_COUNT_MAX];
        size_t prev[GRAPH_NODE_COUNT_MAX];

        if (!dijkstra(edges, s_current_node, dest_node, dist, prev)) {
            // Spec 6.2: no route available, report the error and wait
            ESP_LOGE(TAG, "No route from %c to %c", itochar(s_current_node), itochar(dest_node));
            executor_set_error(job.req.id, ADAM_ERR_NO_PATH);
            continue;
        }

        size_t order[GRAPH_NODE_COUNT_MAX];
        int n = build_path(dest_node, prev, order, GRAPH_NODE_COUNT_MAX);
        for (size_t i = n; i < GRAPH_NODE_COUNT_MAX; i++) {
            order[i] = (size_t)-1;
        }

        char label[2] = {job.reset ? 'R' : job.req.node, '\0'};
        enqueue_nav(order, n, &job, label);

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

esp_err_t path_resolver_init(void) {
    s_path_queue = xQueueCreate(1, sizeof(nav_path_t));
    if (s_path_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(path_resolver, "Task Route Finder", 3072, NULL, 2, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool path_resolver_get_nav(nav_path_t* out) {
    if (s_path_queue == NULL) {
        return false;
    }
    return xQueueReceive(s_path_queue, out, 0) == pdTRUE;
}

void path_resolver_set_current(size_t node) {
    s_current_node = node;
}

size_t path_resolver_current(void) {
    return s_current_node;
}

void path_resolver_flush_paths(void) {
    if (s_path_queue != NULL) {
        xQueueReset(s_path_queue);
    }
}
