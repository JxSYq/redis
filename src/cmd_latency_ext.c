/*
 * Copyright (c) 2024, Redis Ltd.
 * All rights reserved.
 *
 * Extended command-level latency tracking for INFO commandstats.
 */

#include "server.h"
#include "hdr_histogram.h"
#include "cmd_latency_ext.h"

/* Redis 4/5: provide our own updateCommandLatencyHistogram (not available). */
static void updateCommandLatencyHistogram(struct hdr_histogram **h, int64_t duration) {
    if (duration < LATENCY_HISTOGRAM_MIN_VALUE) duration = LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration > LATENCY_HISTOGRAM_MAX_VALUE) duration = LATENCY_HISTOGRAM_MAX_VALUE;
    if (*h == NULL) {
        if (hdr_init(LATENCY_HISTOGRAM_MIN_VALUE, LATENCY_HISTOGRAM_MAX_VALUE,
                      LATENCY_HISTOGRAM_PRECISION, h) != 0)
            return;
    }
    hdr_record_value(*h, duration);
}



/* =========================================================================
 * Recording functions
 * ========================================================================= */

/* Record a latency sample into the 1-minute approximate histogram.
 * Uses a 120-second tumbling window: the histogram is reset every
 * 120 seconds, providing an approximation of "recent 1-2 minutes"
 * rather than a precise 60-second sliding window.  Ring-buffer
 * stats (calls_minute, usec_minute, etc.) are exact sliding windows. */
static void recordCommandLatencyHistogram1m(struct hdr_histogram **histogram,
                                      long long *reset_time, long long duration_ns) {
    long long now = server.unixtime;

    /* Tumbling window: discard histogram every 120 seconds. */
    if (*histogram != NULL && now - *reset_time >= 120) {
        hdr_close(*histogram);
        *histogram = NULL;
    }

    if (*histogram == NULL) {
        /* Cooldown check: if a previous allocation failed, don't retry
         * for 120 seconds to avoid OOM-thrashing and log storms.
         * *reset_time == 0 means the histogram was never allocated
         * (startup / resetstate), so we always allow the first attempt. */
        if (*reset_time != 0 && now - *reset_time < 120)
            return;

        if (hdr_init(LATENCY_HISTOGRAM_MIN_VALUE, LATENCY_HISTOGRAM_MAX_VALUE,
                      LATENCY_HISTOGRAM_PRECISION, histogram) != 0 ||
            *histogram == NULL) {
            *reset_time = now;  /* start cooldown: retry in 120s */
            serverLog(LL_WARNING, "Failed to allocate 1-minute latency histogram, will retry in 120s");
            return;
        }
        *reset_time = now;
    }

    if (duration_ns < LATENCY_HISTOGRAM_MIN_VALUE)
        duration_ns = LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration_ns > LATENCY_HISTOGRAM_MAX_VALUE)
        duration_ns = LATENCY_HISTOGRAM_MAX_VALUE;
    hdr_record_value(*histogram, duration_ns);
}

static void updateCommandLatencyWindow(commandLatencyWindow *window, long long duration_us) {
    long long now = server.unixtime;
    int idx = now % CMD_LATENCY_WINDOW_1M_SLOTS;
    commandLatencySlot *slot = &window->slots[idx];

    if (slot->slot_time != now) {
        memset(slot, 0, sizeof(*slot));
        slot->slot_time = now;
        slot->min_usec = duration_us;
        slot->max_usec = duration_us;
    } else {
        if (duration_us < slot->min_usec)
            slot->min_usec = duration_us;
        if (duration_us > slot->max_usec)
            slot->max_usec = duration_us;
    }
    slot->calls++;
    slot->sum_usec += duration_us;
}

/* Return the appropriate aggregate category for a command.
 * Priority: ADMIN -> other; (!READ && !WRITE) -> other; READ -> read; else -> write. */
static commandLatencyAggregate *getCategoryAggregate(struct redisCommand *cmd) {
    int is_admin = (cmd->flags & CMD_ADMIN) != 0;
    int is_read  = (cmd->flags & CMD_READONLY) != 0;
    int is_write = (cmd->flags & CMD_WRITE) != 0;
    if (is_admin || (!is_read && !is_write))
        return &server.cmd_latency_aggr_other;
    if (is_read)
        return &server.cmd_latency_aggr_read;
    return &server.cmd_latency_aggr_write;
}

/* Update a single aggregate (all / read / write / other) with one latency sample. */
static void updateLatencyAggregate(commandLatencyAggregate *aggr, long long duration_us) {
    if (aggr->calls == 0 || duration_us < aggr->usec_min)
        aggr->usec_min = duration_us;
    if (duration_us > aggr->usec_max)
        aggr->usec_max = duration_us;
    aggr->calls++;
    aggr->microseconds += duration_us;
    updateCommandLatencyHistogram(&aggr->latency_histogram, duration_us * 1000);
    updateCommandLatencyWindow(&aggr->latency_window, duration_us);
    recordCommandLatencyHistogram1m(&aggr->latency_histogram_1m,
                                     &aggr->latency_histogram_1m_reset_time,
                                     duration_us * 1000);
}

void updateCommandExtendedLatencyStats(struct redisCommand *cmd, long long duration_us) {
    if (!server.command_latency_tracking_enabled || !cmd) return;
    if (cmd->latency_ext.latency_samples == 0 || duration_us < cmd->latency_ext.usec_min)
        cmd->latency_ext.usec_min = duration_us;
    if (duration_us > cmd->latency_ext.usec_max)
        cmd->latency_ext.usec_max = duration_us;
    cmd->latency_ext.latency_samples++;

    /* Uses Redis native updateCommandLatencyHistogram() for all-time
     * histogram allocation; OOM behavior intentionally follows native
     * latency-tracking -- the 1-minute histogram has its own cooldown
     * (see recordCommandLatencyHistogram1m). */

    updateCommandLatencyHistogram(&cmd->latency_ext.latency_histogram_all, duration_us * 1000);

    recordCommandLatencyHistogram1m(&cmd->latency_ext.latency_histogram_1m,
                                     &cmd->latency_ext.latency_histogram_1m_reset_time,
                                     duration_us * 1000);

    updateCommandLatencyWindow(&cmd->latency_ext.latency_window, duration_us);

    updateLatencyAggregate(&server.cmd_latency_aggr_all, duration_us);
    updateLatencyAggregate(getCategoryAggregate(cmd), duration_us);
}

void updateCommandAggregateError(struct redisCommand *cmd, int is_rejected) {
    if (!server.command_latency_tracking_enabled) return;

    server.cmd_latency_aggr_all.rejected_calls += (is_rejected ? 1 : 0);
    server.cmd_latency_aggr_all.failed_calls += (is_rejected ? 0 : 1);

    /* If a caller explicitly passes cmd == NULL (defensive / future use),
     * fall back to "other" category.  Normal commandstats error accounting
     * passes a known command. */
    commandLatencyAggregate *aggr;
    if (cmd)
        aggr = getCategoryAggregate(cmd);
    else
        aggr = &server.cmd_latency_aggr_other;

    aggr->rejected_calls += (is_rejected ? 1 : 0);
    aggr->failed_calls += (is_rejected ? 0 : 1);
}

/* =========================================================================
 * Reset / initialization
 * ========================================================================= */

void resetCommandLatencyWindow(commandLatencyWindow *window) {
    memset(window, 0, sizeof(*window));
}

void resetCommandLatencyAggregate(commandLatencyAggregate *aggr) {
    if (!aggr) return;
    /* Fast path: skip heavy reset when tracking data was never recorded. */
    if (aggr->calls == 0 &&
        aggr->rejected_calls == 0 &&
        aggr->failed_calls == 0 &&
        aggr->latency_histogram == NULL &&
        aggr->latency_histogram_1m == NULL)
        return;
    aggr->microseconds = 0;
    aggr->calls = 0;
    aggr->rejected_calls = 0;
    aggr->failed_calls = 0;
    aggr->usec_min = 0;
    aggr->usec_max = 0;
    resetCommandLatencyWindow(&aggr->latency_window);
    if (aggr->latency_histogram) {
        hdr_close(aggr->latency_histogram);
        aggr->latency_histogram = NULL;
    }
    if (aggr->latency_histogram_1m) {
        hdr_close(aggr->latency_histogram_1m);
        aggr->latency_histogram_1m = NULL;
    }
    aggr->latency_histogram_1m_reset_time = 0;
}

void resetCommandExtendedTracking(struct redisCommand *c) {
    if (!c) return;
    /* Fast path: skip reset when tracking was never used for this command.
     * Avoids ~2400-byte memset per command (~480KB for 200 commands)
     * which causes cache pollution on systems under load. */
    if (c->latency_ext.latency_samples == 0 &&
        c->latency_ext.latency_histogram_all == NULL &&
        c->latency_ext.latency_histogram_1m == NULL)
        return;
    c->latency_ext.usec_min = 0;
    c->latency_ext.usec_max = 0;
    c->latency_ext.latency_samples = 0;
    resetCommandLatencyWindow(&c->latency_ext.latency_window);
    if (c->latency_ext.latency_histogram_all) {
        hdr_close(c->latency_ext.latency_histogram_all);
        c->latency_ext.latency_histogram_all = NULL;
    }
    if (c->latency_ext.latency_histogram_1m) {
        hdr_close(c->latency_ext.latency_histogram_1m);
        c->latency_ext.latency_histogram_1m = NULL;
    }
    c->latency_ext.latency_histogram_1m_reset_time = 0;
}

/*
 * Zero-initialize extended tracking fields for a single command.
 * Must only be called on freshly-created commands before any latency
 * histogram can be allocated -- calling on an already-live command
 * will leak existing histograms.  Use resetCommandExtendedTracking()
 * for already-live commands.
 */
void initCommandExtendedTracking(struct redisCommand *c) {
    if (!c) return;
    memset(&c->latency_ext, 0, sizeof(c->latency_ext));
}

/*
 * Recursive variant: resets this command and all nested subcommands.
 * Use when the caller does NOT iterate subcommands on its own
 * (e.g., applyCommandLatencyTrackingConfig, module command cleanup).
 * In contexts where the caller already recurses into subcommands
 * (e.g., resetCommandTableStats), use the non-recursive version above.
 */
void resetCommandExtendedTrackingRecursive(struct redisCommand *c) {
    if (!c) return;
    resetCommandExtendedTracking(c);
    /* Redis 4/5: no subcommands_dict; recursion not needed. */
}

int applyCommandLatencyTrackingConfig(const char **err) {
    UNUSED(err);
    int enabled = server.command_latency_tracking_enabled;
    if (server.command_latency_tracking_prev_enabled == enabled &&
        server.command_latency_tracking_prev_enabled != -1)
        return 1;
    server.command_latency_tracking_prev_enabled = enabled;

    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL)
        resetCommandExtendedTrackingRecursive(dictGetVal(de));
    dictReleaseIterator(di);

    resetCommandLatencyAggregate(&server.cmd_latency_aggr_all);
    resetCommandLatencyAggregate(&server.cmd_latency_aggr_read);
    resetCommandLatencyAggregate(&server.cmd_latency_aggr_write);
    resetCommandLatencyAggregate(&server.cmd_latency_aggr_other);
    return 1;
}

/* Zero-initialize extended tracking fields for all commands (called once at
 * startup to ensure static command-table fields are clean). */
static void initCommandTrackingFields(dict *commands) {
    dictIterator *di = dictGetSafeIterator(commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *c = dictGetVal(de);
        initCommandExtendedTracking(c);
        /* Redis 4/5: no subcommands_dict; recursion not needed. */
    }
    dictReleaseIterator(di);

}
void initCommandLatencyTracking(void) {
    initCommandTrackingFields(server.commands);
    server.command_latency_tracking_prev_enabled = server.command_latency_tracking_enabled;
}

int commandLatencyTrackingIsEnabled(void) {
    return server.command_latency_tracking_enabled;
}

int commandLatencyAggregateHasData(commandLatencyAggregate *aggr) {
    return aggr && (aggr->calls > 0 || aggr->rejected_calls > 0 || aggr->failed_calls > 0);
}

/* =========================================================================
 * INFO output helpers
 * ========================================================================= */

void computeCommandLatencyWindowStats(commandLatencyWindow *window, int window_slots,
                                       long long *calls, long long *sum_usec,
                                       long long *min_usec, long long *max_usec) {
    serverAssert(window_slots <= CMD_LATENCY_WINDOW_1M_SLOTS);
    long long now = server.unixtime;
    long long c = 0, s = 0, mn = LLONG_MAX, mx = 0;

    for (int i = 0; i < window_slots; i++) {
        long long ts = now - i;
        int idx = ts % CMD_LATENCY_WINDOW_1M_SLOTS;
        if (idx < 0) idx += CMD_LATENCY_WINDOW_1M_SLOTS;
        commandLatencySlot *slot = &window->slots[idx];
        if (slot->slot_time != ts) continue;
        c += slot->calls;
        s += slot->sum_usec;
        if (slot->min_usec < mn) mn = slot->min_usec;
        if (slot->max_usec > mx) mx = slot->max_usec;
    }

    *calls = c;
    *sum_usec = s;
    *min_usec = (c > 0) ? mn : 0;
    *max_usec = mx;
}

/* Append 5-second and 1-minute window statistics to the info string. */
static sds appendWindowStats(sds info, commandLatencyWindow *window,
                              struct hdr_histogram *hist_1m) {
    long long c5, s5, mn5, mx5;
    computeCommandLatencyWindowStats(window, CMD_LATENCY_WINDOW_5S_SLOTS,
                                      &c5, &s5, &mn5, &mx5);
    long long avg5 = (c5 > 0) ? (s5 / c5) : 0;
    info = sdscatprintf(info, ",calls_5s=%lld,usec_5s=%lld,usec_min_5s=%lld,usec_avg_5s=%lld,usec_max_5s=%lld",
                        c5, s5, mn5, avg5, mx5);

    long long c60, s60, mn60, mx60;
    computeCommandLatencyWindowStats(window, CMD_LATENCY_WINDOW_1M_SLOTS,
                                      &c60, &s60, &mn60, &mx60);
    long long avg60 = (c60 > 0) ? (s60 / c60) : 0;
    long long p95_1m = 0, p99_1m = 0;
    if (c60 > 0 && hist_1m && hist_1m->total_count > 0) {
        p95_1m = hdr_value_at_percentile(hist_1m, 95.0) / 1000;
        p99_1m = hdr_value_at_percentile(hist_1m, 99.0) / 1000;
    }
    info = sdscatprintf(info, ",calls_minute=%lld,usec_minute=%lld,usec_min_minute=%lld,usec_avg_minute=%lld,usec_max_minute=%lld,usec_p95_minute=%lld,usec_p99_minute=%lld",
                        c60, s60, mn60, avg60, mx60, p95_1m, p99_1m);
    return info;
}

sds formatCommandLatencyExtendedStats(sds info, struct redisCommand *cmd) {
    long long p95 = 0, p99 = 0;
    if (cmd->latency_ext.latency_histogram_all && cmd->latency_ext.latency_histogram_all->total_count > 0) {
        p95 = hdr_value_at_percentile(cmd->latency_ext.latency_histogram_all, 95.0) / 1000;
        p99 = hdr_value_at_percentile(cmd->latency_ext.latency_histogram_all, 99.0) / 1000;
    }

    info = sdscatprintf(info, ",usec_min=%lld,usec_max=%lld,usec_p95=%lld,usec_p99=%lld",
                        cmd->latency_ext.usec_min, cmd->latency_ext.usec_max, p95, p99);

    info = appendWindowStats(info, &cmd->latency_ext.latency_window,
                              cmd->latency_ext.latency_histogram_1m);
    return info;
}

sds formatAggregateLatencyStats(sds info, const char *name,
                                 commandLatencyAggregate *aggr) {
    if (!aggr) return info;
    long long p95 = 0, p99 = 0;
    if (aggr->latency_histogram && aggr->latency_histogram->total_count > 0) {
        p95 = (long long)(hdr_value_at_percentile(aggr->latency_histogram, 95.0) / 1000);
        p99 = (long long)(hdr_value_at_percentile(aggr->latency_histogram, 99.0) / 1000);
    }

    info = sdscatprintf(info,
        "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f"
        ",rejected_calls=%lld,failed_calls=%lld"
        ",usec_min=%lld,usec_max=%lld,usec_p95=%lld,usec_p99=%lld",
        name, aggr->calls, aggr->microseconds,
        (aggr->calls == 0) ? 0.0 : ((double)aggr->microseconds / aggr->calls),
        aggr->rejected_calls, aggr->failed_calls,
        aggr->usec_min, aggr->usec_max, p95, p99);

    info = appendWindowStats(info, &aggr->latency_window,
                              aggr->latency_histogram_1m);

    info = sdscat(info, "\r\n");
    return info;
}
