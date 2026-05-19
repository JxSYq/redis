#include "server.h"
#include "histogram.h"

#include <string.h>

#define LOG_BUCKET_COUNT 30

typedef struct {
    uint64_t counts[LOG_BUCKET_COUNT];
    uint64_t total_count;
} logBucketHistogram;

static inline int logBucketIndex(uint64_t usec) {
    if (usec == 0) return 0;
    int idx = 63 - __builtin_clzll(usec);
    if (idx >= LOG_BUCKET_COUNT) idx = LOG_BUCKET_COUNT - 1;
    return idx;
}

static void* logBucket_create(void) {
    logBucketHistogram *h = zcalloc(sizeof(logBucketHistogram));
    return h;
}

static void logBucket_destroy(void *impl) {
    zfree(impl);
}

static void logBucket_record(void *impl, uint64_t usec) {
    logBucketHistogram *h = impl;
    h->counts[logBucketIndex(usec)]++;
    h->total_count++;
}

static uint64_t logBucket_percentile(void *impl, double pct) {
    logBucketHistogram *h = impl;
    if (h->total_count == 0) return 0;
    uint64_t threshold = (uint64_t)(h->total_count * pct);
    if (threshold == 0) threshold = 1;
    uint64_t cumulative = 0;
    for (int i = 0; i < LOG_BUCKET_COUNT; i++) {
        cumulative += h->counts[i];
        if (cumulative >= threshold) {
            return (i == 0) ? 1 : (1ULL << i);
        }
    }
    return 1ULL << (LOG_BUCKET_COUNT - 1);
}

static void logBucket_merge_from(void *dst, void *src) {
    logBucketHistogram *d = dst, *s = src;
    for (int i = 0; i < LOG_BUCKET_COUNT; i++)
        d->counts[i] += s->counts[i];
    d->total_count += s->total_count;
}

static void logBucket_reset(void *impl) {
    logBucketHistogram *h = impl;
    memset(h->counts, 0, sizeof(h->counts));
    h->total_count = 0;
}

static size_t logBucket_mem_usage(void *impl) {
    UNUSED(impl);
    return sizeof(logBucketHistogram);
}

histogramVtable histogram_engines[HISTOGRAM_TYPE_COUNT] = {
    [HISTOGRAM_LOG_BUCKET] = {
        .name       = "logbucket",
        .create     = logBucket_create,
        .destroy    = logBucket_destroy,
        .record     = logBucket_record,
        .percentile = logBucket_percentile,
        .merge_from = logBucket_merge_from,
        .reset      = logBucket_reset,
        .mem_usage  = logBucket_mem_usage,
    },
};
