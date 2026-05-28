#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
/*
 * Copyright (c) 2024, Redis Ltd.
 * All rights reserved.
 *
 * Extended command-level latency tracking for INFO commandstats.
 *
 * Controlled by "command-latency-tracking" config (default: no).
 * All operations run in the main thread -- no locking needed.
 *
 * Design decisions (may help reviewers avoid re-raising settled questions):
 *
 * 1. latency_histogram_all is separate from the native latency_histogram
 *    (latency-tracking feature).  Reusing the native histogram would couple
 *    the two features and cause data leakage when toggling configs.
 *    The extra ~40 KiB per command is accepted as cost of independence.
 *
 * 2. 1-minute P95/P99 is a 120-second tumbling HdrHistogram -- NOT a precise
 *    60-second sliding window.  calls_minute / usec_minute / usec_min_minute /
 *    usec_avg_minute / usec_max_minute are exact sliding-window values from
 *    the ring buffer.  The tumbling histogram avoids O(n*m) histogram
 *    reconstruction overhead and the per-second-histogram memory explosion
 *    (2.4 MiB/command for 60 per-second HdrHistograms).
 *
 * 3. The commandLatencyWindow (2.4 KiB) is embedded in commandLatencyExtData
 *    struct rather than heap-allocated.  This avoids a pointer chase and
 *    allocation overhead on every command call, at the cost of ~1 MiB total
 *    for ~400 commands even when tracking is disabled.
 *
 * 4. Aggregate lines (cmdstat_-, cmdstat_r, cmdstat_w, cmdstat_o) use
 *    CMD_ADMIN / CMD_READONLY / CMD_WRITE flags for classification.
 *    Priority: admin > (!read && !write -> other) > read > write.
 *    This means DEBUG OBJECT (CMD_ADMIN|CMD_READONLY) -> other, not read.
 *
 * 5. Toggling config with CONFIG SET resets all command-latency-tracking
 *    extended statistics. Redis native commandstats (calls/usec/failed/
 *    rejected) are NOT affected by the toggle.
 *
 * 6. usec_per_call uses %.2f, matching Redis 7.2 native format.
 */

#ifndef __CMD_LATENCY_EXT_H
#define __CMD_LATENCY_EXT_H

#include "sds.h"

/* Forward declarations -- full definitions are not needed at header level. */
struct hdr_histogram;
struct redisCommand;
struct dict;

/* --------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

#define CMD_LATENCY_WINDOW_5S_SLOTS  5
#define CMD_LATENCY_WINDOW_1M_SLOTS 60

/* Redis 6 portability macros */
#define CMD_LATENCY_CMD_NAME(cmd)       ((cmd)->name)
#define CMD_LATENCY_HAS_SUBCOMMANDS(cmd) (0)
#define CMD_LATENCY_CMD_SUBCOMMANDS(cmd) (NULL)

/* Fallback definitions for Redis versions that lack these macros. */
#ifndef LATENCY_HISTOGRAM_MIN_VALUE
#define LATENCY_HISTOGRAM_MIN_VALUE 1L
#endif
#ifndef LATENCY_HISTOGRAM_MAX_VALUE
#define LATENCY_HISTOGRAM_MAX_VALUE 1000000000L
#endif
#ifndef LATENCY_HISTOGRAM_PRECISION
#define LATENCY_HISTOGRAM_PRECISION 2
#endif

typedef struct {
    long long slot_time;     /* epoch-seconds timestamp of this slot */
    long long calls;         /* number of calls in this second */
    long long sum_usec;      /* total microseconds in this second */
    long long min_usec;      /* min latency (usec) in this second */
    long long max_usec;      /* max latency (usec) in this second */
} commandLatencySlot;

typedef struct {
    commandLatencySlot slots[CMD_LATENCY_WINDOW_1M_SLOTS];
} commandLatencyWindow;

typedef struct {
    long long microseconds;
    long long calls;           /* calls with measured execution latency; rejected/deferred errors excluded */
    long long rejected_calls;
    long long failed_calls;
    long long usec_min;
    long long usec_max;
    commandLatencyWindow latency_window;
    struct hdr_histogram *latency_histogram;
    struct hdr_histogram *latency_histogram_1m;
    /* Last histogram allocation/reset timestamp.
     * When histogram is NULL: serves as allocation-retry cooldown timestamp
     *   (if non-zero, do not retry within 120 seconds).
     * When histogram is non-NULL: marks when the 120-second tumbling window
     *   started; histogram is discarded when (now - this) >= 120. */
    long long latency_histogram_1m_reset_time;
} commandLatencyAggregate;

/*
 * Per-command extended latency data -- packed into a single struct so
 * struct redisCommand only needs ONE extra field for the whole feature,
 * making porting to Redis 4/5/6 much simpler (add one field instead of seven).
 *
 * Lifecycle:
 *   - At server init:    initCommandTrackingFields() calls
 *                         initCommandExtendedTracking() for each native command.
 *   - Module cmd create:  moduleCreateCommandProxy() calls
 *                         initCommandExtendedTracking().
 *   - At runtime:         histograms lazily allocated on first record.
 *   - At CONFIG SET:      applyCommandLatencyTrackingConfig() calls
 *                         resetCommandExtendedTrackingRecursive() -- frees
 *                         histograms (both enable->disable AND disable->enable).
 *   - At RESETSTAT:       resetCommandTableStats() calls non-recursive
 *                         resetCommandExtendedTracking() while itself walking
 *                         the command table recursively.
 *   - At module unload:   moduleUnregisterCommands() calls
 *                         resetCommandExtendedTrackingRecursive().
 *   - At shutdown:        process exit reclaims memory (typical for Redis).
 *
 * Memory: ~82.4 KiB per command when both histograms are allocated
 *         (2.4KB window + ~40KB all-time histogram + ~40KB 1m histogram).
 *         For ~400 commands: ~32 MiB total when fully active.
 */
typedef struct {
    long long usec_min;
    long long usec_max;
    long long latency_samples;
    commandLatencyWindow latency_window;
    struct hdr_histogram *latency_histogram_all;
    struct hdr_histogram *latency_histogram_1m;
    long long latency_histogram_1m_reset_time; /* see commandLatencyAggregate above */
} commandLatencyExtData;

/* --------------------------------------------------------------------------
 * Core API -- recording
 * -------------------------------------------------------------------------- */

/*
 * updateCommandExtendedLatencyStats -- records latency for commands that
 *   actually executed (including commands that returned errors).
 *   Updates: per-cmd min/max/usec, histogram_all, histogram_1m, ring buffer,
 *            and 2 aggregate categories (all + r/w/o).
 *   Does NOT touch rejected_calls / failed_calls -- those are handled by
 *   updateCommandAggregateError (see below). The two functions have
 *   completely disjoint responsibilities and CAN be called together
 *   safely -- no double counting.
 */
void updateCommandExtendedLatencyStats(struct redisCommand *cmd, long long duration_us);

/*
 * updateCommandAggregateError -- records a failed/rejected command on the
 *   aggregate categories only. Does NOT touch calls, microseconds, or
 *   any latency fields (no double counting with updateCommandExtendedLatencyStats).
 *
 *   is_rejected: 1 = rejected_calls++,   0 = failed_calls++
 *
 *   For normal error commands in call(), updateCommandExtendedLatencyStats
 *   is also called afterwards to record the command's latency. For "deferred
 *   error" commands (dummy_error path that never actually executed), only
 *   updateCommandAggregateError is called -- no latency to record.
 */
void updateCommandAggregateError(struct redisCommand *cmd, int is_rejected);

/* --------------------------------------------------------------------------
 * Core API -- reset / init
 * -------------------------------------------------------------------------- */

void resetCommandLatencyWindow(commandLatencyWindow *window);
void resetCommandLatencyAggregate(commandLatencyAggregate *aggr);
void resetCommandExtendedTracking(struct redisCommand *c);
void resetCommandExtendedTrackingRecursive(struct redisCommand *c);
void initCommandExtendedTracking(struct redisCommand *c);
int applyCommandLatencyTrackingConfig(const char **err);
void initCommandLatencyTracking(void);

/* --------------------------------------------------------------------------
 * Core API -- INFO output
 * -------------------------------------------------------------------------- */

void computeCommandLatencyWindowStats(commandLatencyWindow *window, int window_slots,
                                       long long *calls, long long *sum_usec,
                                       long long *min_usec, long long *max_usec);
sds formatCommandLatencyExtendedStats(sds info, struct redisCommand *cmd);
sds formatAggregateLatencyStats(sds info, const char *name,
                                 commandLatencyAggregate *aggr);

/*
 * Returns 1 if per-command lines should include extended fields and
 * aggregate category lines should be appended.  The caller still owns
 * the decision of WHEN to call the formatting helpers -- typically
 * from genRedisInfoStringCommandStats only when this returns true.
 */
int commandLatencyTrackingIsEnabled(void);
int commandLatencyAggregateHasData(commandLatencyAggregate *aggr);

/* --------------------------------------------------------------------------
 * Macro wrappers -- reduce coupling at call sites for portability.
 *
 * Use these macros instead of calling the functions directly in main-path
 * code (server.c, blocked.c, etc.).  They encapsulate the enabled-check
 * and can be redefined as no-ops when porting to older Redis versions.
 * -------------------------------------------------------------------------- */

#define CMD_LATENCY_STATS_UPDATE(cmd, duration)                               \
    do {                                                                      \
        if (unlikely(server.command_latency_tracking_enabled))                \
            updateCommandExtendedLatencyStats(cmd, duration);                 \
    } while (0)

#define CMD_LATENCY_ERROR_UPDATE(cmd, is_rejected)                            \
    do {                                                                      \
        if (unlikely(server.command_latency_tracking_enabled))                \
            updateCommandAggregateError(cmd, is_rejected);                    \
    } while (0)

#endif /* __CMD_LATENCY_EXT_H */
