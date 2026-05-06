
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

#endif /* __AUDIT_H */
