#ifndef SRC_EXECUTOR_H
#define SRC_EXECUTOR_H

#include "esp_err.h"
#include "mqtt.h"

esp_err_t init_executor(void);

// Current robot status (published wire code, docs/06-protocol.md)
adam_status_t executor_status(void);

/* Enter the ERROR state (LINE_LOST / NODE_TIMEOUT / NO_PATH):
 * publish adam/srv/error (1 byte code) + status ERROR, halt the motor,
 * flush every queue; the robot waits for an admin reset (spec 7.2). */
void executor_set_error(adam_error_t reason);

#endif // SRC_EXECUTOR_H
