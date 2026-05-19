#ifndef __HISTOGRAM_H
#define __HISTOGRAM_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    HISTOGRAM_LOG_BUCKET = 0,
    HISTOGRAM_TYPE_COUNT
} histogramType;

typedef struct histogram {
    histogramType type;
    void *impl;
} histogram;

typedef struct histogramVtable {
    const char *name;
    void*    (*create)(void);
    void     (*destroy)(void *impl);
    void     (*record)(void *impl, uint64_t usec);
    uint64_t (*percentile)(void *impl, double pct);
    void     (*merge_from)(void *dst, void *src);
    void     (*reset)(void *impl);
    size_t   (*mem_usage)(void *impl);
} histogramVtable;

extern histogramVtable histogram_engines[HISTOGRAM_TYPE_COUNT];

static inline void histogramRecord(histogram *h, uint64_t usec) {
    histogram_engines[h->type].record(h->impl, usec);
}

static inline uint64_t histogramPercentile(histogram *h, double pct) {
    return histogram_engines[h->type].percentile(h->impl, pct);
}

static inline void histogramMergeFrom(histogram *dst, histogram *src) {
    histogram_engines[dst->type].merge_from(dst->impl, src->impl);
}

static inline void histogramReset(histogram *h) {
    histogram_engines[h->type].reset(h->impl);
}

static inline void histogramDestroy(histogram *h) {
    histogram_engines[h->type].destroy(h->impl);
}

static inline size_t histogramMemUsage(histogram *h) {
    return histogram_engines[h->type].mem_usage(h->impl);
}

#endif
