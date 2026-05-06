
#ifndef __AUDIT_H
#define __AUDIT_H

#include "sds.h"

#define AUDIT_CLIENT_TYPE_NORMAL "0"
#define AUDIT_CLIENT_TYPE_SLAVE  "1"
#define AUDIT_CLIENT_TYPE_PUBSUB "2"
#define AUDIT_CLIENT_TYPE_MASTER "3"

typedef struct auditLogEntry {
    /* Business fields */
    long long time;             /* Command arrival time (nanoseconds since epoch) */
    sds instance_id;            /* Redis instance identifier */
    sds proxy_addr;             /* Redis listen address (ip:port) */
    sds server_addr;            /* Actual server address */
    sds role;                   /* "master" or "slave" */
    sds client_addr;            /* Client remote address (ip:port) */
    sds client_type;            /* Client type code */
    sds user;                   /* Authenticated username */
    int db;                     /* Selected database number */
    sds command_name;           /* Command name (e.g. SET) */
    sds command_type;           /* Command type (e.g. string) */
    int num_keys;               /* Number of keys */
    sds *command_keys;          /* Array of key names */
    sds command_param;          /* Full command param string */
    long long use_time;         /* Command execution time (microseconds) */
    sds extend;                 /* Extension info (isTrans for transactions) */

    /* Serialized output */
    sds raw;                    /* JSON serialized audit log line */
} auditLogEntry;

/* Queue API */
void auditLogQueueInit(int capacity);
int  auditLogQueuePush(auditLogEntry *entry);
void auditLogQueuePop(auditLogEntry **entry);
void auditLogQueueDestroy(void);
int  auditLogQueueLen(void);

/* Thread lifecycle */
void auditLogThreadStart(void);
void auditLogThreadStop(void);

/* Queue rebuild for capacity change */
void auditLogQueueRebuild(int new_capacity);

/* File operations */
int  auditLogFileOpen(const char *path);
void auditLogFileWrite(const char *line, size_t len);
void auditLogFileClose(void);
void auditLogFileSwitch(const char *newPath);

/* Configuration update callbacks */
int auditLogEnabledUpdate(int val, int prev, const char **err);
int auditLogPathUpdate(char *val, char *prev, const char **err);
int auditLogQueueLengthUpdate(long long val, long long prev, const char **err);
int auditCustomerCommandListUpdate(char *val, char *prev, const char **err);

/* Command type mapping */
void auditCommandTypeInit(void);
const char *auditGetCommandType(const char *cmdName);

/* Command keys extraction */
sds *auditExtractKeys(client *c, int *numkeys);
void auditFreeKeys(sds *keys, int numkeys);

/* Command param construction, truncation and encryption */
sds auditBuildCommandParam(client *c, sds *keys, int numkeys, int encryptEnabled);

/* Audit entry lifecycle */
auditLogEntry *auditCreateEntry(client *c);
sds auditEntryToJSON(auditLogEntry *entry);
void auditFreeEntry(auditLogEntry *entry);

/* Command filtering and audit trigger */
void auditRebuildCustomerCommandDict(void);
int auditShouldLog(client *c);
void auditLogCommand(client *c);
void auditLogTransactionCommand(client *c, long long prev_err_count);
long long auditNanoTime(void);

/*
 * Extension points for future enhancements:
 *
 * 1. Log output target abstraction:
 *    Currently only local file is supported. To add network output (syslog,
 *    kafka, etc.), implement an auditLogWriter interface with function
 *    pointers for open/write/close and register via auditLogSetWriter().
 *
 * 2. Command blacklist:
 *    Add audit-log-blacklist config to exclude specific write commands.
 *    Implement auditShouldLog() blacklist check before whitelist check.
 *
 * 3. Sampling rate:
 *    Add audit-log-sample-rate config (0-100). Implement random sampling
 *    in auditLogCommand() to log only a percentage of commands.
 *
 * 4. Log rotation:
 *    Add audit-log-max-size and audit-log-rotate-count configs.
 *    Implement size-based rotation in auditLogFileWrite().
 *
 * 5. Custom fields via extend:
 *    The 'extend' field provides structured extension. Parsers can check
 *    for 'isTrans' and other future values.
 *
 * 6. Slow log threshold:
 *    Add audit-log-min-use-time config. Filter entries by use_time
 *    threshold in auditLogCommand().
 *
 * 7. Client filtering:
 *    Add audit-log-client-filter config (address or user based).
 *    Check client_addr/user in auditShouldLog().
 */

#endif /* __AUDIT_H */
