/*
 * =====================================================================================
 *
 *       Filename:  idb_redis.h
 *
 *    Description:  redis for idb
 *
 *        Version:  1.0
 *        Created:  2013/03/25
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Riceball LEE(snowyu.lee@gmail.com)
 *
 * =====================================================================================
 */
#ifndef __IDB_REDIS_H
#define __IDB_REDIS_H

#include "../deps/idb/src/idb_helpers.h"

//subkeys [keyPath [pattern [count [skipCount]]]]
void subkeysCommand(redisClient *c);

/* iDB */
int flushAllToIDB(); //save all changes to iDB.
int iDBSaveBackground();
void backgroundIDBSaveDoneHandler(int exitcode, int bysignal);


robj *lookupKeyOnIDB(redisDb *db, robj *key);
int deleteOnIDB(redisDb *db, robj *key);
void setKeyOnIDB(redisDb *db, robj *key);

sds getKeyNameOnIDB(int dbId, sds key);
dictEntry *getDictEntryOnDirtyKeys(redisDb *db, robj *key);


#endif
