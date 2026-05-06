
#include "server.h"
#include <pthread.h>
#include <signal.h>

/* Ring buffer queue for asynchronous audit logging */
typedef struct auditLogQueue {
    auditLogEntry **entries;    /* Circular buffer of entry pointers */
    int capacity;               /* Maximum number of entries */
    int head;                   /* Index of the first (oldest) entry to pop */
    int tail;                   /* Index where the next entry will be placed */
    int count;                  /* Current number of entries in the queue */
    pthread_mutex_t lock;       /* Mutex for thread safety */
    pthread_cond_t cond;        /* Condition variable to signal consumer */
    int should_stop;            /* Flag: thread should exit */
} auditLogQueue;

static auditLogQueue *audit_queue = NULL;
static pthread_t audit_log_thread;
static int audit_thread_running = 0;

/* Thread function prototype */
static void *auditLogThreadMain(void *arg);

/* Redefine REDIS_THREAD_STACK_SIZE for this file */
#ifndef REDIS_THREAD_STACK_SIZE
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
#endif

/* --------------------------------------------------------------------------
 * Queue operations
 * -------------------------------------------------------------------------- */

/* Initialize the queue with a given capacity. Must be called before
 * any other queue operation. */
void auditLogQueueInit(int capacity) {
    if (audit_queue != NULL) return;

    audit_queue = zmalloc(sizeof(auditLogQueue));
    audit_queue->entries = zcalloc(sizeof(auditLogEntry*) * capacity);
    audit_queue->capacity = capacity;
    audit_queue->head = 0;
    audit_queue->tail = 0;
    audit_queue->count = 0;
    audit_queue->should_stop = 0;
    pthread_mutex_init(&audit_queue->lock, NULL);
    pthread_cond_init(&audit_queue->cond, NULL);
}

/* Non-blocking push. Returns 1 on success, 0 if the queue is full. */
int auditLogQueuePush(auditLogEntry *entry) {
    if (audit_queue == NULL) return 0;

    pthread_mutex_lock(&audit_queue->lock);

    if (audit_queue->count >= audit_queue->capacity) {
        pthread_mutex_unlock(&audit_queue->lock);
        server.audit_log_abort_count++;
        return 0;
    }

    audit_queue->entries[audit_queue->tail] = entry;
    audit_queue->tail = (audit_queue->tail + 1) % audit_queue->capacity;
    audit_queue->count++;
    server.audit_log_record_count++;

    /* Signal consumer that there is data available */
    pthread_cond_signal(&audit_queue->cond);
    pthread_mutex_unlock(&audit_queue->lock);
    return 1;
}

/* Blocking pop. Waits for data or stop signal. Sets *entry to NULL
 * if the thread should stop. */
void auditLogQueuePop(auditLogEntry **entry) {
    if (audit_queue == NULL) {
        *entry = NULL;
        return;
    }

    pthread_mutex_lock(&audit_queue->lock);

    while (audit_queue->count == 0 && !audit_queue->should_stop) {
        pthread_cond_wait(&audit_queue->cond, &audit_queue->lock);
    }

    if (audit_queue->should_stop && audit_queue->count == 0) {
        pthread_mutex_unlock(&audit_queue->lock);
        *entry = NULL;
        return;
    }

    *entry = audit_queue->entries[audit_queue->head];
    audit_queue->entries[audit_queue->head] = NULL;
    audit_queue->head = (audit_queue->head + 1) % audit_queue->capacity;
    audit_queue->count--;

    pthread_mutex_unlock(&audit_queue->lock);
}

/* Return the current queue length (number of entries). */
int auditLogQueueLen(void) {
    if (audit_queue == NULL) return 0;
    int len;
    pthread_mutex_lock(&audit_queue->lock);
    len = audit_queue->count;
    pthread_mutex_unlock(&audit_queue->lock);
    return len;
}

/* Destroy the queue, freeing all remaining entries. */
void auditLogQueueDestroy(void) {
    if (audit_queue == NULL) return;

    pthread_mutex_lock(&audit_queue->lock);

    /* Free all remaining entries */
    for (int i = 0; i < audit_queue->count; i++) {
        int idx = (audit_queue->head + i) % audit_queue->capacity;
        if (audit_queue->entries[idx] != NULL) {
            sdsfree(audit_queue->entries[idx]->raw);
            zfree(audit_queue->entries[idx]);
            audit_queue->entries[idx] = NULL;
        }
    }

    pthread_mutex_unlock(&audit_queue->lock);

    /* Destroy mutex and cond after we are done accessing them */
    pthread_mutex_destroy(&audit_queue->lock);
    pthread_cond_destroy(&audit_queue->cond);

    zfree(audit_queue->entries);
    zfree(audit_queue);
    audit_queue = NULL;
}

/* Rebuild the queue with a new capacity. Old entries are drained first.
 * This should be called with the consumer thread either stopped or
 * with care taken to avoid races. */
void auditLogQueueRebuild(int new_capacity) {
    if (audit_queue == NULL || new_capacity <= 0) return;

    pthread_mutex_lock(&audit_queue->lock);

    /* Allocate new buffer */
    auditLogEntry **new_entries = zcalloc(sizeof(auditLogEntry*) * new_capacity);
    int new_count = 0;
    int new_tail = 0;

    /* Copy existing entries, dropping oldest if over capacity */
    int entries_to_copy = (audit_queue->count < new_capacity) ? 
                          audit_queue->count : new_capacity;

    for (int i = 0; i < entries_to_copy; i++) {
        int old_idx = (audit_queue->head + i) % audit_queue->capacity;
        /* Keep oldest entries if we need to drop */
        if (audit_queue->count > new_capacity) {
            old_idx = (audit_queue->head + (audit_queue->count - new_capacity) + i) % audit_queue->capacity;
        }
        new_entries[new_tail] = audit_queue->entries[old_idx];
        new_tail = (new_tail + 1) % new_capacity;
        new_count++;
    }

    /* For entries that were dropped, free them */
    if (audit_queue->count > new_capacity) {
        int dropped = audit_queue->count - new_capacity;
        for (int i = 0; i < dropped; i++) {
            int idx = (audit_queue->head + i) % audit_queue->capacity;
            if (audit_queue->entries[idx] != NULL) {
                sdsfree(audit_queue->entries[idx]->raw);
                zfree(audit_queue->entries[idx]);
            }
        }
        server.audit_log_abort_count += dropped;
    }

    zfree(audit_queue->entries);
    audit_queue->entries = new_entries;
    audit_queue->capacity = new_capacity;
    audit_queue->head = 0;
    audit_queue->tail = new_tail;
    audit_queue->count = new_count;

    pthread_mutex_unlock(&audit_queue->lock);
}

/* --------------------------------------------------------------------------
 * Consumer thread
 * -------------------------------------------------------------------------- */

static void *auditLogThreadMain(void *arg) {
    UNUSED(arg);
    sigset_t sigset;

    redis_set_thread_title("audit_log");

    makeThreadKillable();

    /* Block SIGALRM so only the main thread receives watchdog signals */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in audit thread: %s", strerror(errno));

    while (1) {
        auditLogEntry *entry;
        auditLogQueuePop(&entry);

        if (entry == NULL) {
            /* Should stop and queue is empty */
            break;
        }

        /* Process the entry: just free it for now (Commit 2).
         * File writing will be added in Commit 3. */
        sdsfree(entry->raw);
        zfree(entry);
    }

    return NULL;
}

void auditLogThreadStart(void) {
    if (audit_thread_running) return;

    /* Initialize the queue with configured capacity */
    auditLogQueueInit(server.audit_log_queue_length);

    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;

    /* Set stack size */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    if (!stacksize) stacksize = 1;
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    if (pthread_create(&thread, &attr, auditLogThreadMain, NULL) != 0) {
        serverLog(LL_WARNING,
            "Fatal: Can't initialize Audit Log thread.");
        exit(1);
    }
    audit_log_thread = thread;
    audit_thread_running = 1;
}

void auditLogThreadStop(void) {
    if (!audit_thread_running || audit_queue == NULL) return;

    /* Signal the thread to stop */
    pthread_mutex_lock(&audit_queue->lock);
    audit_queue->should_stop = 1;
    pthread_cond_signal(&audit_queue->cond);
    pthread_mutex_unlock(&audit_queue->lock);

    /* Wait for thread to finish (drain remaining items) */
    pthread_join(audit_log_thread, NULL);
    audit_thread_running = 0;

    /* Clean up the queue */
    auditLogQueueDestroy();
}
