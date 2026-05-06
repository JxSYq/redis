
#include "server.h"
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

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
static FILE *audit_log_file = NULL;

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
 * any other queue operation. Enforces a minimum capacity of 1 to
 * prevent undefined behavior from zero-capacity ring buffer. */
void auditLogQueueInit(int capacity) {
    if (audit_queue != NULL) return;
    if (capacity < 1) capacity = 1;

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

/* Non-blocking push. Returns 1 on success, 0 if the queue is full.
 * The caller retains ownership of the entry on failure (must free it). */
int auditLogQueuePush(auditLogEntry *entry) {
    if (audit_queue == NULL || entry == NULL) return 0;

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
        /* When shrinking, skip the oldest (dropped) entries */
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
 * File utility
 * -------------------------------------------------------------------------- */

/* Recursively create all parent directories needed for the given file path.
 * Returns 0 on success, -1 on failure. Does not fail if directories already
 * exist. Directory permissions are 0755 (subject to umask). */
int auditEnsureDir(const char *filepath) {
    if (filepath == NULL || *filepath == '\0') return -1;

    char *path = zstrdup(filepath);
    int ret = 0;

    /* Walk the path, creating each directory level. Skip the root '/'. */
    for (char *p = (path[0] == '/') ? path + 1 : path; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (path[0] != '\0') {
                struct stat st;
                if (stat(path, &st) == -1) {
                    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
                        ret = -1;
                        goto cleanup;
                    }
                } else if (!S_ISDIR(st.st_mode)) {
                    ret = -1;
                    goto cleanup;
                }
            }
            *p = '/';
        }
    }

cleanup:
    zfree(path);
    return ret;
}

/* --------------------------------------------------------------------------
 * File operations
 * -------------------------------------------------------------------------- */

/* Open the audit log file in append mode with permissions 0600.
 * Creates parent directories if they don't exist.
 * Returns 1 on success, 0 on failure. */
int auditLogFileOpen(const char *path) {
    if (!path || path[0] == '\0') return 0;

    /* Ensure parent directories exist before opening file */
    auditEnsureDir(path);

    FILE *f = fopen(path, "a");
    if (!f) {
        serverLog(LL_WARNING,
            "Audit log: Failed to open file: %s", path);
        return 0;
    }

    /* Set file permissions to 0600 (owner read/write only) */
    int fd = fileno(f);
    fchmod(fd, S_IRUSR | S_IWUSR);

    audit_log_file = f;
    return 1;
}

/* Write a line to the audit log file. */
void auditLogFileWrite(const char *line, size_t len) {
    if (audit_log_file == NULL) return;
    if (fwrite(line, 1, len, audit_log_file) != len) {
        serverLog(LL_WARNING, "Audit log: Failed to write to log file");
    }
    fflush(audit_log_file);
}

/* Close the current audit log file. */
void auditLogFileClose(void) {
    if (audit_log_file) {
        fclose(audit_log_file);
        audit_log_file = NULL;
    }
}

/* Switch to a new log file path. Closes the old file and opens the new one. */
void auditLogFileSwitch(const char *newPath) {
    auditLogFileClose();
    if (newPath && newPath[0] != '\0') {
        auditLogFileOpen(newPath);
    }
}

/* --------------------------------------------------------------------------
 * Configuration update callbacks
 * -------------------------------------------------------------------------- */

/* Called when audit-log-enabled is changed via CONFIG SET */
int auditLogEnabledUpdate(int val, int prev, const char **err) {
    UNUSED(prev);
    UNUSED(err);
    if (!val) {
        /* When disabled, close the file. The consumer thread will
         * still drain the queue but won't write to file. */
        auditLogFileClose();
    }
    return 1;
}

/* Called when audit-log-path is changed via CONFIG SET */
int auditLogPathUpdate(char *val, char *prev, const char **err) {
    UNUSED(prev);
    UNUSED(err);
    auditLogFileSwitch(val);
    return 1;
}

/* Called when audit-log-queue-length is changed via CONFIG SET */
int auditLogQueueLengthUpdate(long long val, long long prev, const char **err) {
    UNUSED(prev);
    UNUSED(err);
    if (val > 0 && val != prev) {
        auditLogQueueRebuild((int)val);
    }
    return 1;
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
            break;
        }

        if (server.audit_log_enabled) {
            auditLogFileWrite(entry->raw, sdslen(entry->raw));
        }
        sdsfree(entry->raw);
        zfree(entry);
    }

    return NULL;
}

void auditLogThreadStart(void) {
    if (audit_thread_running) return;

    /* Initialize the queue with configured capacity */
    auditLogQueueInit(server.audit_log_queue_length);

    /* Open log file if path is configured */
    if (server.audit_log_path && server.audit_log_path[0] != '\0') {
        auditLogFileOpen(server.audit_log_path);
    }

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
