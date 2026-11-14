#ifndef MINIKV_WAL_H
#define MINIKV_WAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Write-Ahead Log (WAL): binary record encoding, buffered group-commit append,
 * configurable fsync policy (always / everysec / no), and crash-tolerant replay.
 *
 * Module boundary: wal knows nothing about sockets, RESP, cmd, or db.
 * Startup replay uses a callback (wal_replay_fn) so there is no wal -> db cycle.
 */

enum wal_fsync {
    WAL_FSYNC_ALWAYS,
    WAL_FSYNC_EVERYSEC,
    WAL_FSYNC_NO,
};

enum wal_op {
    WAL_OP_SET = 1,
    WAL_OP_DEL = 2,
    WAL_OP_EXPIRE = 3,
    WAL_OP_PERSIST = 4,
};

struct wal_record {
    enum wal_op op;
    const char *key;
    size_t key_len;
    const char *val;
    size_t val_len;
    int64_t expire_at_ms;
};

/*
 * Callback invoked by wal_replay() for each valid record in the log.
 * rec->key and rec->val are borrowed slices valid for the duration of the call.
 * Return 0 on success, or -1 on fatal error (e.g. out-of-memory) to abort replay.
 */
typedef int (*wal_replay_fn)(const struct wal_record *rec, void *ctx);

struct wal;

/*
 * Opens or creates the WAL file at path (without starting the background fsync
 * thread yet) and replays all valid existing records into apply_fn(rec, ctx).
 *
 * If the file is newly created, writes the 8-byte header ("MKVWAL" + v1) and
 * fsyncs both the file and its parent directory.
 * If the tail record is truncated or fails its CRC32C checksum, truncates the
 * file back to the last valid record offset and logs a warning.
 * After replay succeeds, starts the background fsync thread if policy is
 * WAL_FSYNC_EVERYSEC.
 *
 * Caller owns the returned pointer and must close it with wal_close().
 * Returns NULL on unrecoverable I/O or header error (already logged).
 */
struct wal *wal_open(const char *path, enum wal_fsync policy, wal_replay_fn apply_fn, void *ctx);

/*
 * Flushes any buffered records, stops and joins the background fsync thread (if
 * running), performs a final fdatasync(), closes the file, and frees w.
 * Safe to call with NULL.
 */
void wal_close(struct wal *w);

/*
 * Returns 1 if the WAL has entered read-only mode due to a prior write() or
 * fdatasync() failure, 0 otherwise.
 */
int wal_is_readonly(struct wal *w);

/*
 * Encodes a mutation record into the in-memory WAL buffer (no syscall).
 * Returns 0 on success, -1 if in read-only mode or out of memory.
 */
int wal_append_set(struct wal *w, const char *key, size_t key_len, const char *val, size_t val_len,
                   int64_t expire_at_ms);
int wal_append_del(struct wal *w, const char *key, size_t key_len);
int wal_append_expire(struct wal *w, const char *key, size_t key_len, int64_t expire_at_ms);
int wal_append_persist(struct wal *w, const char *key, size_t key_len);

/*
 * Group-commit flush called once per event-loop iteration before client replies
 * are sent. Writes the in-memory WAL buffer to the kernel and applies the
 * configured fsync policy.
 *
 * Returns 0 normally (even if a write() failed and entered read-only mode), or
 * -1 if policy is WAL_FSYNC_ALWAYS and fdatasync() failed (caller must refuse
 * to acknowledge the batch and shut down cleanly).
 */
int wal_flush(struct wal *w);

/*
 * Periodic timer hook (called at 10 Hz from cmd_tick). Under WAL_FSYNC_EVERYSEC,
 * signals the background fsync thread once per second when new bytes have been
 * written since the last fsync.
 */
void wal_tick(struct wal *w);

/* Pure CRC32C (Castagnoli) helper exposed for unit testing. */
uint32_t wal_crc32c(const void *data, size_t len);

#endif
