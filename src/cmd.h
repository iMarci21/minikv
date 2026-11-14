#ifndef MINIKV_CMD_H
#define MINIKV_CMD_H

#include "db.h"
#include "proto.h"
#include "wal.h"

/*
 * Command table and execution dispatch.
 *
 * Validates command names and argument counts, appends write operations to
 * struct wal before mutating struct db, and serializes RESP replies into the
 * caller's output buffer via proto_reply_*.
 */

struct cmd;

/*
 * Allocates a command dispatcher context backed by db and wal (both borrowed;
 * must outlive the returned struct cmd; wal may be NULL for in-memory tests).
 * Caller owns the returned pointer and must free it with cmd_destroy().
 * Returns NULL on allocation failure.
 */
struct cmd *cmd_create(struct db *db, struct wal *wal);

/* Frees c. Safe to call with NULL. */
void cmd_destroy(struct cmd *c);

/*
 * Startup replay callback for wal_open(). Applies one decoded WAL record
 * directly to db (passed as ctx) without logging it again.
 * Returns 0 on success, -1 on out-of-memory.
 */
int cmd_apply_wal(const struct wal_record *rec, void *ctx);

/*
 * Executes one parsed command req and appends one RESP reply to out.
 *
 * Client-level errors (unknown command, wrong number of arguments) are normal
 * protocol replies ("-ERR ...\r\n") and return 0. Returns -1 only if appending
 * to out fails due to out-of-memory.
 */
int cmd_exec(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out);

/*
 * Group-commit flush called once per event-loop iteration before client write
 * buffers are flushed. Returns 0 on success, -1 if an 'always' fsync failed and
 * the server must shut down without acknowledging the batch.
 */
int cmd_flush(struct cmd *c);

/*
 * Periodic timer callback invoked from the event loop. Runs bounded active
 * expiry and the everysec WAL fsync tick.
 */
void cmd_tick(struct cmd *c);

#endif
