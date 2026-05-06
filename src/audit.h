
#ifndef __AUDIT_H
#define __AUDIT_H

#include "sds.h"

/* Audit log entry. At this stage, it only contains the JSON string. */
typedef struct auditLogEntry {
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

/* File utility: create parent directories for a file path */
int auditEnsureDir(const char *filepath);

/* File operations */
int  auditLogFileOpen(const char *path);
void auditLogFileWrite(const char *line, size_t len);
void auditLogFileClose(void);
void auditLogFileSwitch(const char *newPath);

/* Configuration update callbacks */
int auditLogEnabledUpdate(int val, int prev, const char **err);
int auditLogPathUpdate(char *val, char *prev, const char **err);
int auditLogQueueLengthUpdate(long long val, long long prev, const char **err);

#endif /* __AUDIT_H */
