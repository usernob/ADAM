#include "graph.h"
#include "esp_err.h"
#include "portmacro.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char* STR_DIR[4] = {"north", "east", "south", "west"};

// Active graph owner: single RAM copy of the matrix, swapped atomically.
// Double-buffered for reader safety: the retired matrix is only freed on the
// next update, so a reader holding the previous pointer has one refresh cycle
// of grace before the memory goes away.
static uint8_t* s_matrix;
static uint8_t* s_matrix_retired;
static size_t s_count;
static uint32_t s_version; // 0 = graph not ready, see graph_version()
static portMUX_TYPE s_matrix_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t graph_update(const uint8_t* data, size_t count, uint32_t version) {
    if (data == NULL || count == 0 || (count - 1) > (DIST_INF / MAX_EDGE_WEIGHT)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t* copy = malloc(count * count);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, data, count * count);

    portENTER_CRITICAL(&s_matrix_lock);
    uint8_t* expired = s_matrix_retired;
    s_matrix_retired = s_matrix;
    s_matrix = copy;
    s_count = count;
    s_version = version;
    portEXIT_CRITICAL(&s_matrix_lock);

    free(expired);
    return ESP_OK;
}

bool graph_get(const uint8_t** edges, size_t* count) {
    portENTER_CRITICAL(&s_matrix_lock);
    const uint8_t* matrix = s_matrix;
    size_t nodes = s_count;
    portEXIT_CRITICAL(&s_matrix_lock);

    if (matrix == NULL) {
        return false;
    }
    *edges = matrix;
    *count = nodes;
    return true;
}

uint32_t graph_version(void) {
    portENTER_CRITICAL(&s_matrix_lock);
    uint32_t version = s_version;
    portEXIT_CRITICAL(&s_matrix_lock);
    return version;
}

bool graph_ready(void) {
    const uint8_t* edges;
    size_t count;
    return graph_version() != 0 && graph_get(&edges, &count);
}

// Weighted undirected graph, anti-symmetric direction (spec 2.1):
// if i -> j is stored with direction D, then j -> i is stored with
// direction (D + 2) mod 4 and the same weight.
//
// Layout (col, row), row 0 = north side:
//   A(0,0) B(1,0) C(2,0) D(3,0)
//   E(0,1) F(1,1) G(2,1) H(3,1)
//   I(0,2) J(1,2) K(2,2) L(3,2)
//   M(0,3) N(1,3) O(2,3) P(3,3) Q(4,3) R(5,3)
//   S(5,2)
// Rule: max ONE edge per compass direction per node, so a stored
// direction is always unambiguous for the robot (spec 2).
// Notes: no C-G, F-J, K-O lanes (holes); S is a dead end; K is a hub.
// clang-format off
const uint8_t dummy_graph[DUMMY_GRAPH_COUNT][DUMMY_GRAPH_COUNT] = {
// A                              B                              C                              D                              E                              F                              G                              H                              I                              J                              K                              L                              M                              N                              O                              P                              Q                              R                              S
    { 0, EDGE_ENCODE(3, DIR_EAST), 0, 0, EDGE_ENCODE(9, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // A
    { EDGE_ENCODE(3, DIR_WEST), 0, EDGE_ENCODE(30, DIR_EAST), 0, 0, EDGE_ENCODE(1, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // B
    { 0, EDGE_ENCODE(30, DIR_WEST), 0, EDGE_ENCODE(6, DIR_EAST), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // C
    { 0, 0, EDGE_ENCODE(6, DIR_WEST), 0, 0, 0, 0, EDGE_ENCODE(2, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // D

    { EDGE_ENCODE(9, DIR_NORTH), 0, 0, 0, 0, EDGE_ENCODE(26, DIR_EAST), 0, 0, EDGE_ENCODE(5, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // E
    { 0, EDGE_ENCODE(1, DIR_NORTH), 0, 0, EDGE_ENCODE(26, DIR_WEST), 0, EDGE_ENCODE(47, DIR_EAST), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // F
    { 0, 0, 0, 0, 0, EDGE_ENCODE(47, DIR_WEST), 0, EDGE_ENCODE(15, DIR_EAST), 0, 0, EDGE_ENCODE(14, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0, 0 }, // G
    { 0, 0, 0, EDGE_ENCODE(2, DIR_NORTH), 0, 0, EDGE_ENCODE(15, DIR_WEST), 0, 0, 0, 0, EDGE_ENCODE(19, DIR_SOUTH), 0, 0, 0, 0, 0, 0, 0 }, // H

    { 0, 0, 0, 0, EDGE_ENCODE(5, DIR_NORTH), 0, 0, 0, 0, EDGE_ENCODE(2, DIR_EAST), 0, 0, EDGE_ENCODE(21, DIR_SOUTH), 0, 0, 0, 0, 0, 0 }, // I
    { 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(2, DIR_WEST), 0, EDGE_ENCODE(8, DIR_EAST), 0, 0, EDGE_ENCODE(33, DIR_SOUTH), 0, 0, 0, 0, 0 }, // J
    { 0, 0, 0, 0, 0, 0, EDGE_ENCODE(14, DIR_NORTH), 0, 0, EDGE_ENCODE(8, DIR_WEST), 0, EDGE_ENCODE(4, DIR_EAST), 0, 0, 0, 0, 0, 0, 0 }, // K
    { 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(19, DIR_NORTH), 0, 0, EDGE_ENCODE(4, DIR_WEST), 0, 0, 0, 0, EDGE_ENCODE(12, DIR_SOUTH), 0, 0, 0 }, // L

    { 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(21, DIR_NORTH), 0, 0, 0, 0, EDGE_ENCODE(7, DIR_EAST), 0, 0, 0, 0, 0 }, // M
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(33, DIR_NORTH), 0, 0, EDGE_ENCODE(7, DIR_WEST), 0, EDGE_ENCODE(40, DIR_EAST), 0, 0, 0, 0 }, // N
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(40, DIR_WEST), 0, EDGE_ENCODE(6, DIR_EAST), 0, 0, 0 }, // O
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(12, DIR_NORTH), 0, 0, EDGE_ENCODE(6, DIR_WEST), 0, EDGE_ENCODE(10, DIR_EAST), 0, 0 }, // P
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(10, DIR_WEST), 0, EDGE_ENCODE(2, DIR_EAST), 0 }, // Q
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(2, DIR_WEST), 0, EDGE_ENCODE(13, DIR_NORTH) }, // R
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, EDGE_ENCODE(13, DIR_SOUTH), 0 } // S
};
// clang-format on
