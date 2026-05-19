#include "server.h"

/* ---- Lazy initialization ---- */

commandExtStats* initCommandExtStats(void) {
    commandExtStats *es = zcalloc(sizeof(commandExtStats));

    es->usec_min_alltime = UINT64_MAX;
    es->usec_max_alltime = 0;

    histogramType t = server.command_latency_histogram_type;

    es->hist_alltime.type = t;
    es->hist_alltime.impl = histogram_engines[t].create();

    for (int i = 0; i < 5; i++) {
        es->slots_5s[i].usec_min = UINT64_MAX;
    }

    for (int i = 0; i < 12; i++) {
        es->slots_minute[i].usec_min = UINT64_MAX;
        es->slots_minute[i].hist.type = t;
        es->slots_minute[i].hist.impl = histogram_engines[t].create();
    }

    return es;
}

void freeCommandExtStats(commandExtStats *es) {
    if (es == NULL) return;

    histogramDestroy(&es->hist_alltime);
    for (int i = 0; i < 12; i++) {
        histogramDestroy(&es->slots_minute[i].hist);
    }
    zfree(es);
}

void resetCommandExtStats(commandExtStats *es) {
    if (es == NULL) return;

    es->calls_alltime = 0;
    es->usec_alltime = 0;
    es->usec_min_alltime = UINT64_MAX;
    es->usec_max_alltime = 0;
    histogramReset(&es->hist_alltime);

    for (int i = 0; i < 5; i++) {
        es->slots_5s[i].calls    = 0;
        es->slots_5s[i].usec     = 0;
        es->slots_5s[i].usec_min = UINT64_MAX;
        es->slots_5s[i].usec_max = 0;
    }

    for (int i = 0; i < 12; i++) {
        es->slots_minute[i].calls    = 0;
        es->slots_minute[i].usec     = 0;
        es->slots_minute[i].usec_min = UINT64_MAX;
        es->slots_minute[i].usec_max = 0;
        histogramReset(&es->slots_minute[i].hist);
    }
}

/* ---- Command classification ---- */

int classifyCommandCategory(struct redisCommand *cmd) {
    if (cmd->flags & CMD_ADMIN) return 3; /* other */
    if (cmd->flags & CMD_READONLY) return 1; /* read */
    if (cmd->flags & CMD_WRITE) return 2; /* write */
    return 3; /* other */
}

/* ---- Hot path: record command latency ---- */

static void recordExtLatency(commandExtStats *es, uint64_t d) {
    if (es == NULL) return;

    es->calls_alltime++;
    es->usec_alltime += d;

    if (d < es->usec_min_alltime) es->usec_min_alltime = d;
    if (d > es->usec_max_alltime) es->usec_max_alltime = d;
    histogramRecord(&es->hist_alltime, d);

    slot5s *s5 = &es->slots_5s[es->cur_5s_slot];
    s5->calls++;
    s5->usec += d;
    if (d < s5->usec_min) s5->usec_min = d;
    if (d > s5->usec_max) s5->usec_max = d;

    slotMinute *sm = &es->slots_minute[es->cur_minute_slot];
    sm->calls++;
    sm->usec += d;
    if (d < sm->usec_min) sm->usec_min = d;
    if (d > sm->usec_max) sm->usec_max = d;
    histogramRecord(&sm->hist, d);
}

void updateCommandExtLatency(client *c, ustime_t duration) {
    if (!server.command_latency_tracking) return;

    struct redisCommand *cmd = c->cmd;
    uint64_t d = (uint64_t)duration;

    if (cmd->ext_stats == NULL) {
        cmd->ext_stats = initCommandExtStats();
    }

    recordExtLatency(cmd->ext_stats, d);

    int cat = classifyCommandCategory(cmd);
    switch (cat) {
        case 1: recordExtLatency(server.category_read, d); break;
        case 2: recordExtLatency(server.category_write, d); break;
        default: recordExtLatency(server.category_other, d); break;
    }
    recordExtLatency(server.category_all, d);
}

/* ---- Sliding window rotation ---- */

static void rotate5sSlot(commandExtStats *es) {
    es->cur_5s_slot = (es->cur_5s_slot + 1) % 5;
    slot5s *s = &es->slots_5s[es->cur_5s_slot];
    s->calls    = 0;
    s->usec     = 0;
    s->usec_min = UINT64_MAX;
    s->usec_max = 0;
}

static void rotateMinuteSlot(commandExtStats *es) {
    es->cur_minute_slot = (es->cur_minute_slot + 1) % 12;
    slotMinute *m = &es->slots_minute[es->cur_minute_slot];
    m->calls    = 0;
    m->usec     = 0;
    m->usec_min = UINT64_MAX;
    m->usec_max = 0;
    histogramReset(&m->hist);
}

static void rotateAllStats5sSlots(void) {
    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *c = dictGetVal(de);
        if (c->ext_stats) rotate5sSlot(c->ext_stats);
    }
    dictReleaseIterator(di);
    if (server.category_all)  rotate5sSlot(server.category_all);
    if (server.category_read) rotate5sSlot(server.category_read);
    if (server.category_write) rotate5sSlot(server.category_write);
    if (server.category_other) rotate5sSlot(server.category_other);
}

static void rotateAllStatsMinuteSlots(void) {
    dictIterator *di = dictGetSafeIterator(server.commands);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *c = dictGetVal(de);
        if (c->ext_stats) rotateMinuteSlot(c->ext_stats);
    }
    dictReleaseIterator(di);
    if (server.category_all)  rotateMinuteSlot(server.category_all);
    if (server.category_read) rotateMinuteSlot(server.category_read);
    if (server.category_write) rotateMinuteSlot(server.category_write);
    if (server.category_other) rotateMinuteSlot(server.category_other);
}

void cronRotateLatencySlots(void) {
    if (!server.command_latency_tracking) return;

    mstime_t now = mstime();

    if (now - server.last_5s_rotate_time >= 1000) {
        server.last_5s_rotate_time = now;
        rotateAllStats5sSlots();
    }

    if (now - server.last_minute_rotate_time >= 5000) {
        server.last_minute_rotate_time = now;
        rotateAllStatsMinuteSlots();
    }
}

/* ---- Window aggregation for INFO output ---- */

void aggregate5s(commandExtStats *es,
                 uint64_t *calls, uint64_t *usec,
                 uint64_t *min, uint64_t *max)
{
    *calls = 0; *usec = 0;
    *min = UINT64_MAX; *max = 0;

    for (int i = 0; i < 5; i++) {
        *calls += es->slots_5s[i].calls;
        *usec  += es->slots_5s[i].usec;
        if (es->slots_5s[i].usec_min < *min)
            *min = es->slots_5s[i].usec_min;
        if (es->slots_5s[i].usec_max > *max)
            *max = es->slots_5s[i].usec_max;
    }
    if (*calls == 0) *min = 0;
}

sds genCommandExtStatsString(commandExtStats *es, sds info) {
    if (es == NULL) return info;

    uint64_t c5s_calls, c5s_usec, c5s_min, c5s_max;
    aggregate5s(es, &c5s_calls, &c5s_usec, &c5s_min, &c5s_max);
    uint64_t c5s_avg = (c5s_calls == 0) ? 0 : c5s_usec / c5s_calls;

    uint64_t cm_calls, cm_usec, cm_min, cm_max, cm_p95, cm_p99;
    aggregateMinute(es, &cm_calls, &cm_usec, &cm_min, &cm_max, &cm_p95, &cm_p99);
    uint64_t cm_avg = (cm_calls == 0) ? 0 : cm_usec / cm_calls;

    uint64_t usec_min_alltime = (es->calls_alltime == 0) ? 0 : es->usec_min_alltime;
    uint64_t p95 = histogramPercentile(&es->hist_alltime, 0.95);
    uint64_t p99 = histogramPercentile(&es->hist_alltime, 0.99);

    info = sdscatprintf(info,
        ",usec_min=%llu,usec_max=%llu,usec_p95=%llu,usec_p99=%llu"
        ",calls_5s=%llu,usec_5s=%llu,usec_min_5s=%llu,usec_avg_5s=%llu,usec_max_5s=%llu"
        ",calls_minute=%llu,usec_minute=%llu,usec_min_minute=%llu,usec_avg_minute=%llu,usec_max_minute=%llu"
        ",usec_p95_minute=%llu,usec_p99_minute=%llu",
        (unsigned long long)usec_min_alltime,
        (unsigned long long)es->usec_max_alltime,
        (unsigned long long)p95,
        (unsigned long long)p99,
        (unsigned long long)c5s_calls,
        (unsigned long long)c5s_usec,
        (unsigned long long)c5s_min,
        (unsigned long long)c5s_avg,
        (unsigned long long)c5s_max,
        (unsigned long long)cm_calls,
        (unsigned long long)cm_usec,
        (unsigned long long)cm_min,
        (unsigned long long)cm_avg,
        (unsigned long long)cm_max,
        (unsigned long long)cm_p95,
        (unsigned long long)cm_p99);

    return info;
}

void aggregateMinute(commandExtStats *es,
                     uint64_t *calls, uint64_t *usec,
                     uint64_t *min, uint64_t *max,
                     uint64_t *p95, uint64_t *p99)
{
    *calls = 0; *usec = 0;
    *min = UINT64_MAX; *max = 0;

    histogram merged;
    histogramType t = server.command_latency_histogram_type;
    merged.type = t;
    merged.impl = histogram_engines[t].create();

    for (int i = 0; i < 12; i++) {
        *calls += es->slots_minute[i].calls;
        *usec  += es->slots_minute[i].usec;
        if (es->slots_minute[i].usec_min < *min)
            *min = es->slots_minute[i].usec_min;
        if (es->slots_minute[i].usec_max > *max)
            *max = es->slots_minute[i].usec_max;
        histogramMergeFrom(&merged, &es->slots_minute[i].hist);
    }

    if (*calls == 0) {
        *min = 0;
        *p95 = 0;
        *p99 = 0;
    } else {
        *p95 = histogramPercentile(&merged, 0.95);
        *p99 = histogramPercentile(&merged, 0.99);
    }

    histogramDestroy(&merged);
}
