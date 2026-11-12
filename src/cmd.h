#ifndef MINIKV_CMD_H
#define MINIKV_CMD_H

#include "db.h"
#include "proto.h"

/*
 * Command table and execution dispatch.
 *
 * Validates command names and argument counts, mutates or queries struct db,
 * and serializes RESP replies into the caller's output buffer via proto_reply_*.
 * In step 7 struct cmd will also hold a pointer to struct wal.
 */

struct cmd;

/*
 * Allocates a command dispatcher context backed by db (borrowed; must outlive
 * the returned struct cmd). Caller owns the returned pointer and must free it
 * with cmd_destroy(). Returns NULL on allocation failure.
 */
struct cmd *cmd_create(struct db *db);

/* Frees c. Safe to call with NULL. */
void cmd_destroy(struct cmd *c);

/*
 * Executes one parsed command req and appends one RESP reply to out.
 *
 * Client-level errors (unknown command, wrong number of arguments) are normal
 * protocol replies ("-ERR ...\r\n") and return 0. Returns -1 only if appending
 * to out fails due to out-of-memory.
 */
int cmd_exec(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out);

/*
 * Periodic timer callback invoked from the event loop. Runs bounded active
 * expiry (and in step 7, the everysec WAL fsync tick).
 */
void cmd_tick(struct cmd *c);

#endif
