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

// Dijkstra scratch space, resized lazily when the active graph count changes
typedef struct {
    void* block;
    dist_t* dist;
    size_t* prev;
    size_t* order;
    bool* visited;
    char* route;
    size_t route_len;
    size_t count;
} route_ws_t;

static route_ws_t s_ws;

static void* take(char** cur, size_t* left, size_t bytes, size_t align) {
    uintptr_t next = ((uintptr_t)*cur + (align - 1)) & ~(uintptr_t)(align - 1);
    if ((next - (uintptr_t)*cur) > *left) {
        return NULL;
    }
    *left -= next - (uintptr_t)*cur;
    if (bytes > *left) {
        return NULL;
    }
    *left -= bytes;
    *cur = (char*)next + bytes;
    return (void*)next;
}

static bool route_workspace_resize(size_t count) {
    if (count == 0 || count > NAV_MAX_NODES || (count - 1) > (DIST_INF / MAX_EDGE_WEIGHT)) {
        return false;
    }

    if (s_ws.count == count) {
        return true;
    }

    route_ws_t next = {
        .route_len = (count * 5) + 1,
        .count = count,
    };

    size_t need =
        (count * sizeof(size_t) * 2) + (count * sizeof(dist_t)) + count + next.route_len + (sizeof(void*) * 2);

    char* cur = calloc(1, need);
    if (cur == NULL) {
        return false;
    }

    size_t left = need;
    next.block = cur;

    next.prev = take(&cur, &left, count * sizeof(size_t), _Alignof(size_t));
    next.order = take(&cur, &left, count * sizeof(size_t), _Alignof(size_t));
    next.dist = take(&cur, &left, count * sizeof(dist_t), _Alignof(dist_t));
    next.visited = take(&cur, &left, count, 1);
    next.route = take(&cur, &left, next.route_len, 1);

    if (!next.prev || !next.order || !next.dist || !next.visited || !next.route) {
        free(next.block);
        return false;
    }

    route_ws_t old = s_ws;
    s_ws = next;

    if (old.count > 0) {
        free(old.block);
    }

    return true;
}

static int find_min_distance(size_t count, const dist_t dist[restrict count], const bool* visited) {
    dist_t min_distance = DIST_INF;
    int min_node = -1;

    for (size_t i = 0; i < count; i++) {
        if (!visited[i] && dist[i] < min_distance) {
            min_distance = dist[i];
            min_node = i;
        }
    }

    return min_node;
}

static bool dijkstra(size_t count, const uint8_t graph[restrict count * count], size_t source, size_t target,
                     dist_t* dist, size_t* out, bool* visited) {
    if (source >= count || target >= count) {
        return false;
    }

    memset(visited, 0, count);

    for (size_t i = 0; i < count; i++) {
        dist[i] = DIST_INF;
        out[i] = (size_t)-1;
    }

    dist[source] = 0;

    for (size_t step = 0; step < count; step++) {
        int u = find_min_distance(count, dist, visited);

        if (u == -1) {
            break;
        }

        visited[u] = true;

        if ((size_t)u == target) {
            break;
        }

        for (size_t v = 0; v < count; v++) {
            uint8_t edge = graph[((size_t)u * count) + v];

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

    s_ws.route[0] = '\0';
    int off = 0;
    for (int i = 0; i < n && off < (int)s_ws.route_len - 5; i++) {
        off += snprintf(s_ws.route + off, s_ws.route_len - off, i ? " -> %c" : "%c", itochar(order[i]));
    }
    ESP_LOGI(TAG, "Path '%s': %s (recovery=%d)", target, s_ws.route, nav.job.reset);

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
        size_t count = 0;
        const uint8_t* edges = NULL;
        if (!graph_get(&edges, &count) || !route_workspace_resize(count)) {
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
            if (target >= count) {
                ESP_LOGW(TAG, "Target node '%c' is outside the graph (%d nodes)", job.req.node, (int)count);
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

        if (!dijkstra(count, edges, s_current_node, dest_node, s_ws.dist, s_ws.prev, s_ws.visited)) {
            // Spec 6.2: no route available, report the error and wait
            ESP_LOGE(TAG, "No route from %c to %c", itochar(s_current_node), itochar(dest_node));
            executor_set_error(job.req.id, ADAM_ERR_NO_PATH);
            continue;
        }

        int n = build_path(dest_node, s_ws.prev, s_ws.order, count);
        for (size_t i = n; i < count; i++) {
            s_ws.order[i] = (size_t)-1;
        }

        char label[2] = {job.reset ? 'R' : job.req.node, '\0'};
        enqueue_nav(s_ws.order, n, &job, label);

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
