#ifndef SRC_EXECUTOR_H
#define SRC_EXECUTOR_H

#include "esp_err.h"
#include "protocol.h"

esp_err_t executor_init(void);

// Current robot status (published wire code, docs/06-protocol.md)
adam_status_t executor_status(void);

// Replay one coherent retained status/current_pos snapshot. Returns false until
// boot has settled to graph-ready IDLE or fetch-failed ERROR.
bool executor_replay_telemetry(void);

// Enter ERROR (LINE_LOST / NODE_TIMEOUT / NO_PATH): publish [req_id:2][code:1]
// + current_pos + status, halt the motor, flush every queue (spec 7.2/7.3).
// req_id = the job that failed, 0 for an error without a request.
void executor_set_error(uint16_t req_id, adam_error_t reason);

#endif // SRC_EXECUTOR_H
