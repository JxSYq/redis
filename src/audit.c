
#include "server.h"
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

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
 * File operations
 * -------------------------------------------------------------------------- */

/* Open the audit log file in append mode with permissions 0600.
 * Returns 1 on success, 0 on failure. */
int auditLogFileOpen(const char *path) {
    if (!path || path[0] == '\0') return 0;

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
 * Command type mapping
 * -------------------------------------------------------------------------- */

/* Forward declarations for case-insensitive SDS dict operations */
uint64_t dictSdsCaseHash(const void *key);
int dictSdsKeyCaseCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);

static dict *audit_command_type_dict = NULL;

static dictType commandTypeDictType = {
    dictSdsCaseHash,            /* hash function (case-insensitive) */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare (case-insensitive) */
    dictSdsDestructor,          /* key destructor (free SDS keys) */
    NULL,                       /* val destructor (values are string literals) */
    NULL                        /* allow to expand */
};

/* Helper macro to add a command type mapping */
#define ADD_CMD_TYPE(cmd, type) \
    dictAdd(audit_command_type_dict, sdsnew(cmd), (void *)(type))

void auditCommandTypeInit(void) {
    if (audit_command_type_dict != NULL) return;
    audit_command_type_dict = dictCreate(&commandTypeDictType, NULL);

    /* string */
    ADD_CMD_TYPE("APPEND", "string");
    ADD_CMD_TYPE("DECR", "string");
    ADD_CMD_TYPE("DECRBY", "string");
    ADD_CMD_TYPE("GET", "string");
    ADD_CMD_TYPE("GETDEL", "string");
    ADD_CMD_TYPE("GETEX", "string");
    ADD_CMD_TYPE("GETRANGE", "string");
    ADD_CMD_TYPE("GETSET", "string");
    ADD_CMD_TYPE("INCR", "string");
    ADD_CMD_TYPE("INCRBY", "string");
    ADD_CMD_TYPE("INCRBYFLOAT", "string");
    ADD_CMD_TYPE("MGET", "string");
    ADD_CMD_TYPE("MSET", "string");
    ADD_CMD_TYPE("MSETNX", "string");
    ADD_CMD_TYPE("PSETEX", "string");
    ADD_CMD_TYPE("SET", "string");
    ADD_CMD_TYPE("SETEX", "string");
    ADD_CMD_TYPE("SETNX", "string");
    ADD_CMD_TYPE("SETRANGE", "string");
    ADD_CMD_TYPE("STRALGO", "string");
    ADD_CMD_TYPE("STRLEN", "string");
    ADD_CMD_TYPE("SUBSTR", "string");

    /* hash */
    ADD_CMD_TYPE("HDEL", "hash");
    ADD_CMD_TYPE("HEXISTS", "hash");
    ADD_CMD_TYPE("HGET", "hash");
    ADD_CMD_TYPE("HGETALL", "hash");
    ADD_CMD_TYPE("HINCRBY", "hash");
    ADD_CMD_TYPE("HINCRBYFLOAT", "hash");
    ADD_CMD_TYPE("HKEYS", "hash");
    ADD_CMD_TYPE("HLEN", "hash");
    ADD_CMD_TYPE("HMGET", "hash");
    ADD_CMD_TYPE("HMSET", "hash");
    ADD_CMD_TYPE("HRANDFIELD", "hash");
    ADD_CMD_TYPE("HSCAN", "hash");
    ADD_CMD_TYPE("HSET", "hash");
    ADD_CMD_TYPE("HSETNX", "hash");
    ADD_CMD_TYPE("HSTRLEN", "hash");
    ADD_CMD_TYPE("HVALS", "hash");

    /* list */
    ADD_CMD_TYPE("BLMOVE", "list");
    ADD_CMD_TYPE("BLMPOP", "list");
    ADD_CMD_TYPE("BLPOP", "list");
    ADD_CMD_TYPE("BRPOP", "list");
    ADD_CMD_TYPE("BRPOPLPUSH", "list");
    ADD_CMD_TYPE("LINDEX", "list");
    ADD_CMD_TYPE("LINSERT", "list");
    ADD_CMD_TYPE("LLEN", "list");
    ADD_CMD_TYPE("LMOVE", "list");
    ADD_CMD_TYPE("LMPOP", "list");
    ADD_CMD_TYPE("LPOP", "list");
    ADD_CMD_TYPE("LPOS", "list");
    ADD_CMD_TYPE("LPUSH", "list");
    ADD_CMD_TYPE("LPUSHX", "list");
    ADD_CMD_TYPE("LRANGE", "list");
    ADD_CMD_TYPE("LREM", "list");
    ADD_CMD_TYPE("LSET", "list");
    ADD_CMD_TYPE("LTRIM", "list");
    ADD_CMD_TYPE("RPOP", "list");
    ADD_CMD_TYPE("RPOPLPUSH", "list");
    ADD_CMD_TYPE("RPUSH", "list");
    ADD_CMD_TYPE("RPUSHX", "list");

    /* set */
    ADD_CMD_TYPE("SADD", "set");
    ADD_CMD_TYPE("SCARD", "set");
    ADD_CMD_TYPE("SDIFF", "set");
    ADD_CMD_TYPE("SDIFFSTORE", "set");
    ADD_CMD_TYPE("SINTER", "set");
    ADD_CMD_TYPE("SINTERCARD", "set");
    ADD_CMD_TYPE("SINTERSTORE", "set");
    ADD_CMD_TYPE("SISMEMBER", "set");
    ADD_CMD_TYPE("SMEMBERS", "set");
    ADD_CMD_TYPE("SMISMEMBER", "set");
    ADD_CMD_TYPE("SMOVE", "set");
    ADD_CMD_TYPE("SPOP", "set");
    ADD_CMD_TYPE("SRANDMEMBER", "set");
    ADD_CMD_TYPE("SREM", "set");
    ADD_CMD_TYPE("SSCAN", "set");
    ADD_CMD_TYPE("SUNION", "set");
    ADD_CMD_TYPE("SUNIONSTORE", "set");

    /* sorted-set */
    ADD_CMD_TYPE("BZMPOP", "sorted-set");
    ADD_CMD_TYPE("BZPOPMAX", "sorted-set");
    ADD_CMD_TYPE("BZPOPMIN", "sorted-set");
    ADD_CMD_TYPE("ZADD", "sorted-set");
    ADD_CMD_TYPE("ZCARD", "sorted-set");
    ADD_CMD_TYPE("ZCOUNT", "sorted-set");
    ADD_CMD_TYPE("ZDIFF", "sorted-set");
    ADD_CMD_TYPE("ZDIFFSTORE", "sorted-set");
    ADD_CMD_TYPE("ZINCRBY", "sorted-set");
    ADD_CMD_TYPE("ZINTER", "sorted-set");
    ADD_CMD_TYPE("ZINTERCARD", "sorted-set");
    ADD_CMD_TYPE("ZINTERSTORE", "sorted-set");
    ADD_CMD_TYPE("ZLEXCOUNT", "sorted-set");
    ADD_CMD_TYPE("ZMSCORE", "sorted-set");
    ADD_CMD_TYPE("ZMPOP", "sorted-set");
    ADD_CMD_TYPE("ZPOPMAX", "sorted-set");
    ADD_CMD_TYPE("ZPOPMIN", "sorted-set");
    ADD_CMD_TYPE("ZRANDMEMBER", "sorted-set");
    ADD_CMD_TYPE("ZRANGE", "sorted-set");
    ADD_CMD_TYPE("ZRANGEBYLEX", "sorted-set");
    ADD_CMD_TYPE("ZRANGEBYSCORE", "sorted-set");
    ADD_CMD_TYPE("ZRANGESTORE", "sorted-set");
    ADD_CMD_TYPE("ZRANK", "sorted-set");
    ADD_CMD_TYPE("ZREM", "sorted-set");
    ADD_CMD_TYPE("ZREMRANGEBYLEX", "sorted-set");
    ADD_CMD_TYPE("ZREMRANGEBYRANK", "sorted-set");
    ADD_CMD_TYPE("ZREMRANGEBYSCORE", "sorted-set");
    ADD_CMD_TYPE("ZREVRANGE", "sorted-set");
    ADD_CMD_TYPE("ZREVRANGEBYLEX", "sorted-set");
    ADD_CMD_TYPE("ZREVRANGEBYSCORE", "sorted-set");
    ADD_CMD_TYPE("ZREVRANK", "sorted-set");
    ADD_CMD_TYPE("ZSCAN", "sorted-set");
    ADD_CMD_TYPE("ZSCORE", "sorted-set");
    ADD_CMD_TYPE("ZUNION", "sorted-set");
    ADD_CMD_TYPE("ZUNIONSTORE", "sorted-set");

    /* bitmap */
    ADD_CMD_TYPE("BITCOUNT", "bitmap");
    ADD_CMD_TYPE("BITFIELD", "bitmap");
    ADD_CMD_TYPE("BITFIELD_RO", "bitmap");
    ADD_CMD_TYPE("BITOP", "bitmap");
    ADD_CMD_TYPE("BITPOS", "bitmap");
    ADD_CMD_TYPE("GETBIT", "bitmap");
    ADD_CMD_TYPE("SETBIT", "bitmap");

    /* hyperloglog */
    ADD_CMD_TYPE("PFADD", "hyperloglog");
    ADD_CMD_TYPE("PFCOUNT", "hyperloglog");
    ADD_CMD_TYPE("PFDEBUG", "hyperloglog");
    ADD_CMD_TYPE("PFMERGE", "hyperloglog");
    ADD_CMD_TYPE("PFSELFTEST", "hyperloglog");

    /* geo */
    ADD_CMD_TYPE("GEOADD", "geo");
    ADD_CMD_TYPE("GEODIST", "geo");
    ADD_CMD_TYPE("GEOHASH", "geo");
    ADD_CMD_TYPE("GEOPOS", "geo");
    ADD_CMD_TYPE("GEORADIUS", "geo");
    ADD_CMD_TYPE("GEORADIUSBYMEMBER", "geo");
    ADD_CMD_TYPE("GEORADIUSBYMEMBER_RO", "geo");
    ADD_CMD_TYPE("GEORADIUS_RO", "geo");
    ADD_CMD_TYPE("GEOSEARCH", "geo");
    ADD_CMD_TYPE("GEOSEARCHSTORE", "geo");

    /* stream */
    ADD_CMD_TYPE("XACK", "stream");
    ADD_CMD_TYPE("XADD", "stream");
    ADD_CMD_TYPE("XAUTOCLAIM", "stream");
    ADD_CMD_TYPE("XCLAIM", "stream");
    ADD_CMD_TYPE("XDEL", "stream");
    ADD_CMD_TYPE("XGROUP", "stream");
    ADD_CMD_TYPE("XINFO", "stream");
    ADD_CMD_TYPE("XLEN", "stream");
    ADD_CMD_TYPE("XPENDING", "stream");
    ADD_CMD_TYPE("XRANGE", "stream");
    ADD_CMD_TYPE("XREAD", "stream");
    ADD_CMD_TYPE("XREADGROUP", "stream");
    ADD_CMD_TYPE("XREVRANGE", "stream");
    ADD_CMD_TYPE("XSETID", "stream");
    ADD_CMD_TYPE("XTRIM", "stream");

    /* pubsub */
    ADD_CMD_TYPE("PSUBSCRIBE", "pubsub");
    ADD_CMD_TYPE("PUBLISH", "pubsub");
    ADD_CMD_TYPE("PUBSUB", "pubsub");
    ADD_CMD_TYPE("PUNSUBSCRIBE", "pubsub");
    ADD_CMD_TYPE("SPUBLISH", "pubsub");
    ADD_CMD_TYPE("SSUBSCRIBE", "pubsub");
    ADD_CMD_TYPE("SUBSCRIBE", "pubsub");
    ADD_CMD_TYPE("SUNSUBSCRIBE", "pubsub");
    ADD_CMD_TYPE("UNSUBSCRIBE", "pubsub");

    /* scripting */
    ADD_CMD_TYPE("EVAL", "scripting");
    ADD_CMD_TYPE("EVAL_RO", "scripting");
    ADD_CMD_TYPE("EVALSHA", "scripting");
    ADD_CMD_TYPE("EVALSHA_RO", "scripting");
    ADD_CMD_TYPE("FUNCTION", "scripting");
    ADD_CMD_TYPE("SCRIPT", "scripting");

    /* transactions */
    ADD_CMD_TYPE("DISCARD", "transactions");
    ADD_CMD_TYPE("EXEC", "transactions");
    ADD_CMD_TYPE("MULTI", "transactions");
    ADD_CMD_TYPE("UNWATCH", "transactions");
    ADD_CMD_TYPE("WATCH", "transactions");

    /* connection */
    ADD_CMD_TYPE("AUTH", "connection");
    ADD_CMD_TYPE("CLIENT", "connection");
    ADD_CMD_TYPE("ECHO", "connection");
    ADD_CMD_TYPE("HELLO", "connection");
    ADD_CMD_TYPE("PING", "connection");
    ADD_CMD_TYPE("QUIT", "connection");
    ADD_CMD_TYPE("RESET", "connection");
    ADD_CMD_TYPE("SELECT", "connection");

    /* server */
    ADD_CMD_TYPE("ACL", "server");
    ADD_CMD_TYPE("BGREWRITEAOF", "server");
    ADD_CMD_TYPE("BGSAVE", "server");
    ADD_CMD_TYPE("COMMAND", "server");
    ADD_CMD_TYPE("CONFIG", "server");
    ADD_CMD_TYPE("DBSIZE", "server");
    ADD_CMD_TYPE("DEBUG", "server");
    ADD_CMD_TYPE("FAILOVER", "server");
    ADD_CMD_TYPE("FLUSHALL", "server");
    ADD_CMD_TYPE("FLUSHDB", "server");
    ADD_CMD_TYPE("INFO", "server");
    ADD_CMD_TYPE("LASTSAVE", "server");
    ADD_CMD_TYPE("LATENCY", "server");
    ADD_CMD_TYPE("LOLWUT", "server");
    ADD_CMD_TYPE("MEMORY", "server");
    ADD_CMD_TYPE("MODULE", "server");
    ADD_CMD_TYPE("MONITOR", "server");
    ADD_CMD_TYPE("PSYNC", "server");
    ADD_CMD_TYPE("REPLCONF", "server");
    ADD_CMD_TYPE("REPLICAOF", "server");
    ADD_CMD_TYPE("ROLE", "server");
    ADD_CMD_TYPE("SAVE", "server");
    ADD_CMD_TYPE("SHUTDOWN", "server");
    ADD_CMD_TYPE("SLAVEOF", "server");
    ADD_CMD_TYPE("SLOWLOG", "server");
    ADD_CMD_TYPE("SWAPDB", "server");
    ADD_CMD_TYPE("SYNC", "server");
    ADD_CMD_TYPE("TIME", "server");

    /* generic */
    ADD_CMD_TYPE("COPY", "generic");
    ADD_CMD_TYPE("DEL", "generic");
    ADD_CMD_TYPE("DUMP", "generic");
    ADD_CMD_TYPE("EXISTS", "generic");
    ADD_CMD_TYPE("EXPIRE", "generic");
    ADD_CMD_TYPE("EXPIREAT", "generic");
    ADD_CMD_TYPE("EXPIRETIME", "generic");
    ADD_CMD_TYPE("KEYS", "generic");
    ADD_CMD_TYPE("MIGRATE", "generic");
    ADD_CMD_TYPE("MOVE", "generic");
    ADD_CMD_TYPE("OBJECT", "generic");
    ADD_CMD_TYPE("PERSIST", "generic");
    ADD_CMD_TYPE("PEXPIRE", "generic");
    ADD_CMD_TYPE("PEXPIREAT", "generic");
    ADD_CMD_TYPE("PEXPIRETIME", "generic");
    ADD_CMD_TYPE("PTTL", "generic");
    ADD_CMD_TYPE("RANDOMKEY", "generic");
    ADD_CMD_TYPE("RENAME", "generic");
    ADD_CMD_TYPE("RENAMENX", "generic");
    ADD_CMD_TYPE("RESTORE", "generic");
    ADD_CMD_TYPE("SCAN", "generic");
    ADD_CMD_TYPE("SORT", "generic");
    ADD_CMD_TYPE("SORT_RO", "generic");
    ADD_CMD_TYPE("TOUCH", "generic");
    ADD_CMD_TYPE("TTL", "generic");
    ADD_CMD_TYPE("TYPE", "generic");
    ADD_CMD_TYPE("UNLINK", "generic");
    ADD_CMD_TYPE("WAIT", "generic");
    ADD_CMD_TYPE("WAITREPLICAS", "generic");

    /* cluster */
    ADD_CMD_TYPE("ASKING", "cluster");
    ADD_CMD_TYPE("CLUSTER", "cluster");
    ADD_CMD_TYPE("READONLY", "cluster");
    ADD_CMD_TYPE("READWRITE", "cluster");
    ADD_CMD_TYPE("REPLICATE", "cluster");

    /* bf (Bloom Filter) */
    ADD_CMD_TYPE("BF.ADD", "bf");
    ADD_CMD_TYPE("BF.CARD", "bf");
    ADD_CMD_TYPE("BF.EXISTS", "bf");
    ADD_CMD_TYPE("BF.INFO", "bf");
    ADD_CMD_TYPE("BF.INSERT", "bf");
    ADD_CMD_TYPE("BF.LOADCHUNK", "bf");
    ADD_CMD_TYPE("BF.MADD", "bf");
    ADD_CMD_TYPE("BF.MEXISTS", "bf");
    ADD_CMD_TYPE("BF.RESERVE", "bf");
    ADD_CMD_TYPE("BF.SCANDUMP", "bf");

    /* cf (Cuckoo Filter) */
    ADD_CMD_TYPE("CF.ADD", "cf");
    ADD_CMD_TYPE("CF.ADDNX", "cf");
    ADD_CMD_TYPE("CF.COUNT", "cf");
    ADD_CMD_TYPE("CF.DEL", "cf");
    ADD_CMD_TYPE("CF.EXISTS", "cf");
    ADD_CMD_TYPE("CF.INFO", "cf");
    ADD_CMD_TYPE("CF.INSERT", "cf");
    ADD_CMD_TYPE("CF.INSERTNX", "cf");
    ADD_CMD_TYPE("CF.LOADCHUNK", "cf");
    ADD_CMD_TYPE("CF.MEXISTS", "cf");
    ADD_CMD_TYPE("CF.RESERVE", "cf");
    ADD_CMD_TYPE("CF.SCANDUMP", "cf");

    /* json */
    ADD_CMD_TYPE("JSON.ARRAPPEND", "json");
    ADD_CMD_TYPE("JSON.ARRINDEX", "json");
    ADD_CMD_TYPE("JSON.ARRINSERT", "json");
    ADD_CMD_TYPE("JSON.ARRLEN", "json");
    ADD_CMD_TYPE("JSON.ARRPOP", "json");
    ADD_CMD_TYPE("JSON.ARRTRIM", "json");
    ADD_CMD_TYPE("JSON.CLEAR", "json");
    ADD_CMD_TYPE("JSON.DEBUG", "json");
    ADD_CMD_TYPE("JSON.DEL", "json");
    ADD_CMD_TYPE("JSON.FORGET", "json");
    ADD_CMD_TYPE("JSON.GET", "json");
    ADD_CMD_TYPE("JSON.MGET", "json");
    ADD_CMD_TYPE("JSON.NUMINCRBY", "json");
    ADD_CMD_TYPE("JSON.NUMMULTBY", "json");
    ADD_CMD_TYPE("JSON.OBJKEYS", "json");
    ADD_CMD_TYPE("JSON.OBJLEN", "json");
    ADD_CMD_TYPE("JSON.RESP", "json");
    ADD_CMD_TYPE("JSON.SET", "json");
    ADD_CMD_TYPE("JSON.STRAPPEND", "json");
    ADD_CMD_TYPE("JSON.STRLEN", "json");
    ADD_CMD_TYPE("JSON.TOGGLE", "json");
    ADD_CMD_TYPE("JSON.TYPE", "json");

    /* search */
    ADD_CMD_TYPE("FT.AGGREGATE", "search");
    ADD_CMD_TYPE("FT.ALIASADD", "search");
    ADD_CMD_TYPE("FT.ALIASDEL", "search");
    ADD_CMD_TYPE("FT.ALIASUPDATE", "search");
    ADD_CMD_TYPE("FT.ALTER", "search");
    ADD_CMD_TYPE("FT.CONFIG", "search");
    ADD_CMD_TYPE("FT.CREATE", "search");
    ADD_CMD_TYPE("FT.CURSOR", "search");
    ADD_CMD_TYPE("FT.DICTADD", "search");
    ADD_CMD_TYPE("FT.DICTDEL", "search");
    ADD_CMD_TYPE("FT.DICTDUMP", "search");
    ADD_CMD_TYPE("FT.DROPINDEX", "search");
    ADD_CMD_TYPE("FT.EXPLAIN", "search");
    ADD_CMD_TYPE("FT.EXPLAINCLI", "search");
    ADD_CMD_TYPE("FT.INFO", "search");
    ADD_CMD_TYPE("FT.PROFILE", "search");
    ADD_CMD_TYPE("FT.SEARCH", "search");
    ADD_CMD_TYPE("FT.SPELLCHECK", "search");
    ADD_CMD_TYPE("FT.SUGADD", "search");
    ADD_CMD_TYPE("FT.SUGDEL", "search");
    ADD_CMD_TYPE("FT.SUGGET", "search");
    ADD_CMD_TYPE("FT.SUGLEN", "search");
    ADD_CMD_TYPE("FT.SYNDUMP", "search");
    ADD_CMD_TYPE("FT.SYNUPDATE", "search");
    ADD_CMD_TYPE("FT.TAGVALS", "search");

    /* timeseries */
    ADD_CMD_TYPE("TS.ADD", "timeseries");
    ADD_CMD_TYPE("TS.ALTER", "timeseries");
    ADD_CMD_TYPE("TS.CREATE", "timeseries");
    ADD_CMD_TYPE("TS.CREATERULE", "timeseries");
    ADD_CMD_TYPE("TS.DECRBY", "timeseries");
    ADD_CMD_TYPE("TS.DEL", "timeseries");
    ADD_CMD_TYPE("TS.DELETERULE", "timeseries");
    ADD_CMD_TYPE("TS.GET", "timeseries");
    ADD_CMD_TYPE("TS.INCRBY", "timeseries");
    ADD_CMD_TYPE("TS.INFO", "timeseries");
    ADD_CMD_TYPE("TS.MADD", "timeseries");
    ADD_CMD_TYPE("TS.MGET", "timeseries");
    ADD_CMD_TYPE("TS.MRANGE", "timeseries");
    ADD_CMD_TYPE("TS.MREVRANGE", "timeseries");
    ADD_CMD_TYPE("TS.QUERYINDEX", "timeseries");
    ADD_CMD_TYPE("TS.RANGE", "timeseries");
    ADD_CMD_TYPE("TS.REVRANGE", "timeseries");

    /* topk */
    ADD_CMD_TYPE("TOPK.ADD", "topk");
    ADD_CMD_TYPE("TOPK.COUNT", "topk");
    ADD_CMD_TYPE("TOPK.INCRBY", "topk");
    ADD_CMD_TYPE("TOPK.INFO", "topk");
    ADD_CMD_TYPE("TOPK.LIST", "topk");
    ADD_CMD_TYPE("TOPK.QUERY", "topk");
    ADD_CMD_TYPE("TOPK.RESERVE", "topk");
}

const char *auditGetCommandType(const char *cmdName) {
    if (cmdName == NULL || cmdName[0] == '\0') return "undefined";
    if (audit_command_type_dict == NULL) return "undefined";

    sds key = sdsnew(cmdName);
    dictEntry *de = dictFind(audit_command_type_dict, key);
    sdsfree(key);

    if (de) return (const char *)dictGetVal(de);
    return "undefined";
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
