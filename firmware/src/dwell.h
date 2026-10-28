#ifndef SRC_DWELL_H
#define SRC_DWELL_H

#include "esp_err.h"

// Dwell duration on the target node (spec 4: 15 - 30 seconds)
#define DWELL_TIME_MS 15000
/* Hard cap for the whole stop (spec 4): the hand IR may reset the 15 s timer,
 * but never beyond this total. Keeps one stop short and fair (queue is FIFO)
 * and makes sure the robot can never be locked inside DWELL forever. */
#define DWELL_TOTAL_MAX_MS 60000
// Trash lid servo angles
#define DWELL_SERVO_OPEN_DEG 90
#define DWELL_SERVO_CLOSE_DEG 0

esp_err_t dwell_init(void);

/* Close the trash lid now (spec 7.3.1: the lid must be closed whenever the
 * robot enters ERROR). Callable outside dwell_run(), e.g. from Task Executor
 * error handling or from another task. */
void dwell_close_lid(void);

/* Dwell procedure on the target node (spec 4, blocking):
 * 1) open the lid with the servo, 2) run the dwell timer and reset it
 * while the hand IR sensor at the trash opening detects a hand,
 * 3) close the lid again.
 * Called from Task Executor while the robot is standing on the node. */
void dwell_run(void);

#endif // SRC_DWELL_H
