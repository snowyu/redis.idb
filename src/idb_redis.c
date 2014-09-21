/*
 * =====================================================================================
 *
 *       Filename:  idb_redis.c
 *
 *    Description:  redis for idb
 *
 *        Version:  1.0
 *        Created:  2013/03/25
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Riceball LEE (snowyu.lee@gmail.com)
 *
 * =====================================================================================
 */


#include "redis.h"
#include "../deps/idb/src/idb_helpers.h"
#include "../deps/idb/src/isdk_string.h"

#define REDIS_VALUE_MAGIC_FLAG "rEdIs\n"
#define REDIS_VALUE_MAGIC_FLAG_LEN 6
#define IS_IDB_ATTR(vAttr) vAttr[1] != '\0' && (vAttr-1) != vKey && vAttr[-1] != '\\'
//the expired time is Millisecond, so the 1 minute = 1*60*1000
#define IDB_CACHE_TIME -5*60*1000
//the item will be saved to iDB if it's expired time greater than this:
#define IDB_SAVED_EXPIRED_TIME 60*60*1000

static inline int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

static inline int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    return rdbWriteRaw(rdb,&t64,8);
}

static inline long long rdbLoadMillisecondTime(rio *rdb) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return -1;
    return (long long)t64;
}

//join the dbId and key
sds getKeyNameOnIDB(int dbId, sds key)
{
    sds result = key;
    if (dbId != 0) {
        result = sdsempty();
        //the sdscatprintf() will print ".db1/(null)" when key is NULL
        if (key) {
            result = sdscatprintf(result, ".db%d%c%s", dbId, PATH_SEP, key);
        } else {
            result = sdscatprintf(result, ".db%d", dbId);
        }
    }
    return result;
}

static inline int dictReplaceObj(dict *d, robj *key, robj *val) {
    if (val) incrRefCount(val);
    int result = dictUpdate(d, key, val) != NULL;
    if (!result) { //new key
        incrRefCount(key);
        result = dictAdd(d, key, val) == DICT_OK;
    }
    return result;
}

static int dictAppendTo(dict *src, dict *dest) {
    int vErr = REDIS_OK;
    dictIterator *di;
    dictEntry *de;
    if (dictSize(src) != 0)
    {
        di = dictGetSafeIterator(src);
        if (!di) {
            return REDIS_ERR;
        }
        robj *key, *val;
        while((de = dictNext(di)) != NULL) {
            key = dictGetKey(de);
            val = dictGetVal(de);
            if (!dictReplaceObj(dest, key, val)) {
                vErr = REDIS_ERR;
                break;
            }
        }

        dictReleaseIterator(di);
    }
    return vErr;
}

dictEntry *getDictEntryOnDirtyKeys(redisDb *db, robj *key)
{
    dict *d;
    dictEntry *de;
    if (server.idb_child_pid == -1) {
        d = db->dirtyKeys;
        de = dictFind(d, key);
    }
    else {
        d = db->dirtyQueue;
        de = dictFind(d, key);
        if (!de) {
            d = db->dirtyKeys;
            de = dictFind(d, key);
        }
    }
    return de;
}

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/
int existsOnIDB(redisDb *db, robj *key)
{
    int result = 0;
    if (server.iDBEnabled) {
        if (!server.iDBSync) {
            dictEntry *de = getDictEntryOnDirtyKeys(db, key);
            if (de) {
                result = dictGetVal(de) != NULL;
                return result;
            }
        }
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        result = iKeyIsExists(server.iDBPath, vKey, sdslen(vKey));
        if (db->id != 0) sdsfree(vKey);
    }
    return result;
    //
}

static int saveKeyAttrOnIDB(redisDb *db, sds key, const char* attr, robj *val) {
    int vResult = 1;
    rio vRedisIO;
    rioInitWithBuffer(&vRedisIO, sdsempty());
    if (rdbSaveObject(&vRedisIO,val) == -1) vResult = -1;
    sds v = vRedisIO.io.buffer.ptr;
    if (vResult == 1) {
        sds vKey = getKeyNameOnIDB(db->id, key);
        if (iPut(server.iDBPath, vKey, sdslen(vKey), v, sdslen(v), attr, server.iDBType) != 0) vResult = -1;
        if (db->id != 0) sdsfree(vKey);
    }
    sdsfree(v);

    return vResult;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
static int saveKeyValuePairOnIDB(redisDb *db, robj *key, robj *val)
{
    //char* vAttr = strrchr(key->ptr, '.');
    char* vAttr = NULL;
    sds vKey = key->ptr;
    /*
    if (vAttr != NULL) {
        if (IS_IDB_ATTR(vAttr))
            vKey  = sdsnewlen(key->ptr, vAttr - (char*)key->ptr);
        else
            vAttr = NULL;
        //if (saveKeyAttrOnIDB(db, vKey, vAttr, val)< 0)
        //    redisLog(REDIS_WARNING,"iDB Write error saving key attr %s on disk", (char*)key->ptr);
        //sdsfree(vKey);
    }
    */
    long long now = mstime();
    long long vExpiredTime = getExpire(db,key);
    int vResult = (vExpiredTime != -1 && vExpiredTime < now);
    if (!vResult) {
        rio vRedisIO;
        vResult = 1; //store the operation result, default is successful.
        sds vK = getKeyNameOnIDB(db->id, vKey);
        sds v = NULL;
        //if (vAttr == NULL || strncasecmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME)) == 0) {
            if (val->type == REDIS_STRING) {
                val = getDecodedObject(val);
                v = val->ptr;
                if (iPut(server.iDBPath, vK, sdslen(vK), v, sdslen(v), vAttr, server.iDBType) != 0) vResult = -1;
                if (iPut(server.iDBPath, vK, sdslen(vK), "string", 6, IDB_KEY_TYPE_NAME, server.iDBType) != 0) vResult = -1;
                decrRefCount(val);
            } 
            else {
                rioInitWithBuffer(&vRedisIO,sdsempty());
                if (rioWrite(&vRedisIO, REDIS_VALUE_MAGIC_FLAG, REDIS_VALUE_MAGIC_FLAG_LEN)==0) {
                    sdsfree(vRedisIO.io.buffer.ptr);
                    return -1;
                }
                if (vExpiredTime != -1) {
                    if (rdbSaveType(&vRedisIO,REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1) vResult = -1;
                    if (rdbSaveMillisecondTime(&vRedisIO,vExpiredTime) == -1) vResult = -1;
                }
                if (rdbSaveObjectType(&vRedisIO,val) == -1) vResult = -1;
                if (rdbSaveObject(&vRedisIO,val) == -1) vResult = -1;
                v = vRedisIO.io.buffer.ptr;
                if (iPut(server.iDBPath, vK, sdslen(vK), v, sdslen(v), vAttr, server.iDBType) != 0) vResult = -1;
                if (iPut(server.iDBPath, vK, sdslen(vK), "redis", 5, IDB_KEY_TYPE_NAME, server.iDBType) != 0) vResult = -1;
                sdsfree(v);
            }
        /*}
        else if (val->type == REDIS_STRING) { //is attribute:
            val = getDecodedObject(val);
            v = val->ptr;
            if (iPut(server.iDBPath, vK, sdslen(vK), v, sdslen(v), vAttr, server.iDBType) != 0) vResult = -1;
            decrRefCount(val);
        }
        else {
            redisLog(REDIS_WARNING,"the key %s attr must be string type", (char*)key->ptr);
        }*/
        if (db->id != 0) sdsfree(vK);
        if (vAttr) sdsfree(vKey);
        //SDSFreeAndNil(v);
        return vResult;
    }
    else //already expired.
        return 0;
}

void setKeyOnIDB(redisDb *db, robj *key)
{
    if (server.iDBEnabled) {
            dictEntry *de = dictFind(db->dict,key->ptr);
            if (de) {
                //redisLog(REDIS_NOTICE, "set a key:%s", key->ptr);
                //key = dictGetKey(de);
                robj *val = dictGetVal(de);
                if (server.iDBSync) {
                    if (saveKeyValuePairOnIDB(db, key, val) >= 0) {
                        //server.dirty--; //that should be a problem for aof writing!!
                    }
                    else
                        redisLog(REDIS_WARNING,"iDB Write error saving key %s on disk", (char*)key->ptr);
                } else {
                    dict *d = server.idb_child_pid == -1 ? db->dirtyKeys : db->dirtyQueue;
                    dictReplaceObj(d, key, val);
                    //incrRefCount(val);
                    //incrRefCount(key);
                    //if (dictReplace(d, key, val) == 0) //0=old key;1=new key
                    //    decrRefCount(key);
                }
            }
    }
}

int iDBKeyDelete(const sds aDir, const char* aKey, const int aKeyLen)
{
    //sds vKey = NULL;
    //char* vAttr = strrchr(aKey, '.');
    int result = 0;
    /*
    if (vAttr != NULL) {
        if (IS_IDB_ATTR(vAttr))
            vKey  = sdsnewlen(aKey, vAttr - (char*)aKey);
        else
            vAttr = NULL;
    }
    if (vAttr == NULL)
    {*/
        result = iKeyIsExists(server.iDBPath, aKey, aKeyLen);
        if (result == 1)
            iKeyDelete(server.iDBPath, aKey, aKeyLen);
    /*}
    else {
        result = iKeyIsExists(server.iDBPath, vKey, sdslen(vKey));
        if (result == 1)
            result = iDelete(server.iDBPath, vKey, sdslen(vKey), vAttr, server.iDBType);
        sdsfree(vKey);
    }*/
    return result;
}


int deleteOnIDB(redisDb *db, robj *key)
{
    int result = server.iDBEnabled;
    if (result) {
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        if (server.iDBSync) {
            //redisLog(REDIS_NOTICE, "Try deleteOnIDB:%s", vKey);
            result = iDBKeyDelete(server.iDBPath, vKey, sdslen(vKey));
        }
        else {
            //the deleting asynchronous
            dict *d;
            if (server.idb_child_pid == -1)
            {
                d = db->dirtyKeys;
                int vKeyExists = iKeyIsExists(server.iDBPath, vKey, sdslen(vKey));
                if (vKeyExists) { //added/update it to queue
                    result = dictReplaceObj(d, key, NULL);
                    if (!result) result = iDBKeyDelete(server.iDBPath, vKey, sdslen(vKey));
                } else { //the key is not exists in the disk
                    result = dictDelete(d, key) == DICT_OK;
                    if (dictDelete(db->dirtyQueue, key) == DICT_OK)
                        if (!result) result = 1;
                }
            }
            else { //the bgsaving...
                d = db->dirtyQueue;
                //dictEntry *de = dictFind(db->dirtyKeys, key);
                //if (vKeyExists) {
                    //must add to queue always. for lookupKeyOnIDB
                    //result = de && dictGetVal(de) == NULL; //the bgsaving is deleting...
                    //if (!result) {
                        result = dictReplaceObj(d, key, NULL);
                        //if (!result) result = iDBKeyDelete(server.iDBPath, vKey, sdslen(vKey));
                    //}
                //}
                //else { //the key is not exists in the disk
                //    result = de && dictGetVal(de) != NULL; //the bgsaving is adding...
                //    if (result) {
                //        result = dictReplaceObj(d, key, NULL);
                //    }
                //}
            }//*/
            //dictEntry *de = dictFind(d, key);
            //redisAssertWithInfo(NULL, key, dictGetVal(de) == NULL);
            //redisLog(REDIS_NOTICE, "deleteOnIDB:%s", key->ptr);
            //server.dirty++;
        }

        if (db->id != 0) sdsfree(vKey);
    }
    return result;
}

int saveDictToIDB(redisDb *db, dict *d) {
    dictIterator *di = NULL;
    dictEntry *de;
    int vErr = REDIS_OK;
    if (dictSize(d) != 0) {
        di = dictGetSafeIterator(d);
        if (!di) {
            return REDIS_ERR;
        }
        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            robj *key = dictGetKey(de);
            robj *o = dictGetVal(de);
            //long long expire;
            if (o == NULL) {
                //ignore the delete error
                iDBKeyDelete(server.iDBPath, key->ptr, sdslen(key->ptr));
            } else {
                if (saveKeyValuePairOnIDB(db,key, o) == -1) {
                    redisLog(REDIS_WARNING,"iDB Write error saving key %s on disk", (char*)key->ptr);
                    vErr = REDIS_ERR;
                    break;
                }
            }
            //dictDelete(d, keystr);// != DICT_OK)
            server.dirty--;
        }
        dictReleaseIterator(di);
    }
    return vErr;
}
//flush dirtyKeys and dirtyQueue to iDB
int flushAllToIDB() {
    int j;
    size_t count = 0;
    int vErr = REDIS_OK;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dirtyKeys;
        count += dictSize(d);
        vErr = saveDictToIDB(db, d);
        if (vErr == REDIS_OK) {
            dictEmpty(d, NULL);
            d = db->dirtyQueue;
            count += dictSize(d);
            vErr = saveDictToIDB(db, d);
            if (vErr == REDIS_OK) dictEmpty(d, NULL);
        }
        if (vErr == REDIS_ERR) break;
    }
    if (vErr == REDIS_OK) {
        redisLog(REDIS_NOTICE,"IDB saved %lu on disk", count);
        server.idb_lastsave = time(NULL);
        server.idb_lastbgsave_status = REDIS_OK;
    }
    return vErr;
}

//flush dirty keys to iDB
int flushToIDB() {
    int j;
    int vErr = REDIS_OK;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dirtyKeys;
        if (dictSize(d) == 0) continue;
        vErr = saveDictToIDB(db, d);
        if (vErr) break;
        //usleep(1);
    }
    if (vErr == REDIS_OK) {
        redisLog(REDIS_NOTICE,"IDB saved on disk");
        server.idb_lastsave = time(NULL);
        server.idb_lastbgsave_status = REDIS_OK;
    }
    return vErr;
}

static void clearServerDirtyKeys() {
    int j;
    dict *d;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        d = db->dirtyKeys;
        dictEmpty(d, NULL);
        d = db->dirtyQueue;
        db->dirtyQueue = db->dirtyKeys;
        db->dirtyKeys = d;
    }
}

static void restoreServerToDirtyKeys() {
    int j;
    redisDb *db;

    for (j = 0; j < server.dbnum; j++) {
        db = server.db+j;
        dictAppendTo(db->dirtyQueue, db->dirtyKeys);
        dictEmpty(db->dirtyQueue, NULL);
    }
}

int iDBSaveBackground()
{
    pid_t childpid;
    long long start;

    if (server.idb_child_pid != -1) return REDIS_ERR;

    server.idb_dirty_before_bgsave = server.dirty;

    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);

    start = ustime();
    if ((childpid = fork()) == 0) {
        int retval;

        /* Child */
        closeListeningSockets(0);
        redisSetProcTitle("redis-idb-bgsave");
        retval = flushToIDB();
        if (retval == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "IDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
        }
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"IDB Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"IDB Background saving started by pid %d",childpid);
        server.idb_save_time_start = time(NULL);
        server.idb_child_pid = childpid;
        updateDictResizePolicy();
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

/* A background saving child (BGSAVE) terminated its work. Handle this. */
void backgroundIDBSaveDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "IDB Background saving terminated with success");
        server.dirty = server.dirty - server.idb_dirty_before_bgsave;
        server.idb_lastsave = time(NULL);
        server.idb_lastbgsave_status = REDIS_OK;
        clearServerDirtyKeys();
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "IDB Background saving error");
        server.idb_lastbgsave_status = REDIS_ERR;
        restoreServerToDirtyKeys();
    } else {
        redisLog(REDIS_WARNING,
            "IDB Background saving terminated by signal %d", bysignal);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        if (bysignal != SIGUSR1)
            server.idb_lastbgsave_status = REDIS_ERR;
        restoreServerToDirtyKeys();
    }
    server.idb_child_pid = -1;
    server.idb_save_time_last = time(NULL)-server.idb_save_time_start;
    server.idb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}

static robj *rioReadValueFromBuffer(redisDb *db, robj *key, sds vValueBuffer)
{
    robj *result = NULL;
    rio vRedisIO;
    if (strncmp(vValueBuffer, REDIS_VALUE_MAGIC_FLAG, REDIS_VALUE_MAGIC_FLAG_LEN) != 0) {
        redisLog(REDIS_WARNING, "(rioReadValueFromBuffer) value is invalid redis format: %s\n", (char*)key->ptr);
        return NULL;
    }
    rioInitWithBuffer(&vRedisIO,vValueBuffer+REDIS_VALUE_MAGIC_FLAG_LEN);
    int vType =rdbLoadType(&vRedisIO);
    if (vType != -1) {
        int vExpired = 0;
        long long vExpiredTime = -1;
        if (vType == REDIS_RDB_OPCODE_EXPIRETIME_MS) {
            long long now = mstime();
            vExpiredTime = rdbLoadMillisecondTime(&vRedisIO);
            //-1 means load MSecTime error
            vExpired = vExpiredTime == -1 || vExpiredTime < now;
        }
        if (!vExpired) {
            result = rdbLoadObject(vType, &vRedisIO);
            if (result) {
                //robj *o = createObject(REDIS_STRING,vValue);
                //dbAdd(db, key, o);
                sds copy = sdsdup(key->ptr);
                int retval = dictAdd(db->dict, copy, result);
                redisAssertWithInfo(NULL,key,retval == REDIS_OK);
                if (vExpiredTime != -1) setExpire(db, key, vExpiredTime);
                /* Update the access time for the ageing algorithm.
                 * Don't do it if we have a saving child, as this will trigger
                 * a copy on write madness. */
                if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 && server.idb_child_pid == -1)
                    result->lru = server.lruclock;
            }
            else
                redisLog(REDIS_WARNING, "(lookupKeyOnIDB) load value is invalid for key: %s\n", (char*)key->ptr);
        }
        else if (vExpiredTime != -1){
            //remove expired key
            iKeyDelete(server.iDBPath, key->ptr, sdslen(key->ptr));
        }
        else
            redisLog(REDIS_WARNING, "(lookupKeyOnIDB) load expiredTime is invalid for key: %s\n", (char*)key->ptr);
    }
    else
        redisLog(REDIS_WARNING, "(lookupKeyOnIDB) load type is invalid for key: %s\n", (char*)key->ptr);
    return result;
}

//lookup the key on IDB and cache it in the memory if found.
robj *lookupKeyOnIDB(redisDb *db, robj *key)
{
    robj *result = NULL;
    //printf("lookupKeyOnIDB: %s\n", (char*)key->ptr);
    if (server.iDBEnabled) {
        if (server.iDBSync == 0) {
            //dict *d = server.idb_child_pid == -1 ? db->dirtyKeys : db->dirtyQueue;
            //if (server.idb_child_pid == -1)

            dictEntry *de = getDictEntryOnDirtyKeys(db, key);
            if (de) {
                result = dictGetVal(de);
                redisAssertWithInfo(NULL,key,result == NULL);
                if (result) { //the strange is that it's not in the db.dict, only in dirtyKeys, why?
                    sds copy = sdsdup(key->ptr);
                    incrRefCount(result);
                    int retval = dictAdd(db->dict, copy, result);
                    redisAssertWithInfo(NULL,key,retval == REDIS_OK);
                }
                return result;
            }
        }
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        //printf("try iGet:%s\n", vKey);
        //char* vAttr = strrchr(vKey, '.');
        sds vK = vKey;
        /*if (vAttr != NULL) {
           if (IS_IDB_ATTR(vAttr))
                vK  = sdsnewlen(vKey, vAttr - (char*)vKey);
           else
                vAttr = NULL;
        }*/
        sds vValueType = NULL;
        sds vValueBuffer;
        /*bool vIsValue = (vAttr == NULL || strncasecmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME)) == 0);
        if (vIsValue) {*/
            vValueType = iGet(server.iDBPath, vK, sdslen(vK), IDB_KEY_TYPE_NAME, server.iDBType);
            vValueBuffer = iGet(server.iDBPath, vK, sdslen(vK), NULL, server.iDBType);
        /*}
        else { //is attribute:
            vValueBuffer = iGet(server.iDBPath, vK, sdslen(vK), vAttr, server.iDBType);
            vValueType   = sdsnewlen("str",3);
            sdsfree(vK);
        }*/
        //printf("lookupKeyOnIDB Loaded: %s=%s\n", (char*)vKey, vValueBuffer);
        if (db->id != 0) sdsfree(vKey);
        if (vValueBuffer) {
            strToLowerCase(vValueType);
            if (strncmp(vValueType, "redis", 5) == 0) {
                result = rioReadValueFromBuffer(db, key, vValueBuffer);
                sdsfree(vValueBuffer);
            }
            else if (strncmp(vValueType, "str", 3) == 0) {
                result = createObject(REDIS_STRING, vValueBuffer);
            }
            else {
                sdsfree(vValueBuffer);
                redisLog(REDIS_WARNING, "(lookupKeyOnIDB) invalid key type '%s' on %s\n", vValueType, (char*)key->ptr);
            //    addReplyErrorFormat(c,"invalid key type on %s", (char*)key->ptr);
            }
        }
        SDSFreeAndNil(vValueType);
    }
    return result;
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

//subkeys [keyPath [pattern [count [skipCount]]]]
void subkeysCommand(redisClient *c) {
    sds vKeyPath = NULL;
    sds vPattern = NULL;
    long vSkipCount = 0, vCount = 0;
    if (c->argc > 5) { //the arg count = subkeys keyPath pattern count skipCount
        addReply(c,shared.syntaxerr);
        return;
    }

    if (server.iDBEnabled) {
        if (c->argc >= 5)
            if (getLongFromObjectOrReply(c, c->argv[4], &vSkipCount, NULL) != REDIS_OK) return;
        if (c->argc >= 4)
            if (getLongFromObjectOrReply(c, c->argv[3], &vCount, NULL) != REDIS_OK) return;
        vCount = (IDBMaxPageCount > 0 && vCount > IDBMaxPageCount) ? IDBMaxPageCount : vCount;
        if (c->argc >= 3)
            vPattern = c->argv[2]->ptr;
        if (c->argc >= 2)
            vKeyPath = c->argv[1]->ptr;

        unsigned long numkeys = 0;
        void *replylen = addDeferredMultiBulkLength(c);

        if (vPattern) vPattern = (vPattern[0] == '*' && vPattern[1] == '\0') || vPattern[0] == '\0' ? NULL : vPattern;
        sds vK = getKeyNameOnIDB(c->db->id, vKeyPath);
        dStringArray *vResult = iSubkeys(server.iDBPath, vK, sdslen(vK), vPattern,
            vSkipCount, vCount, dkFixed);
        if (c->db->id != 0) sdsfree(vK);
        //redisLog(REDIS_WARNING, "dfffffff:\n");
        if (vResult) {
            sds *vItem;
            robj *vObj;
            darray_foreach(vItem, *vResult) {
//                fprintf(stderr, "got:%s\n", *vItem);
                vObj = createObject(REDIS_STRING, *vItem);
                addReplyBulk(c,vObj);
                numkeys++;
                decrRefCount(vObj);
            }
            darray_free(*vResult);
            zfree(vResult);
        }
    //    sds s = sdsnew(NULL);
    //    s = sdscatprintf(s, "keys.argc=%d, dict.size=%lu", c->argc, dictSize(c->db->dict));
    //    robj *t = createStringObject(s,sdslen(s));
    //    addReplyBulk(c,t);numkeys++;
        setDeferredMultiBulkLength(c,replylen,numkeys);
    }
    else {
        addReplyError(c, "the idb is disabled, you should set idb-enabled to yes first on configuration first.");
    }
}

/* ASET key attr value */
void asetCommand(redisClient *c) {
    robj *key, *attr, *value;
    int vResult = 1;

    if (server.iDBEnabled) {
        key   = c->argv[1];
        attr  = c->argv[2];
        value = c->argv[3];

        sds vKey = getKeyNameOnIDB(c->db->id, key->ptr);
        char* vAttr = attr->ptr;
        if (vAttr) vAttr = vAttr[0] == '\0' ? IDB_VALUE_NAME : vAttr;

        if (iPut(server.iDBPath, vKey, sdslen(vKey), value->ptr, sdslen(value->ptr), attr->ptr, server.iDBType) != 0) vResult = 0;
        if (strncasecmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME)) == 0) {
            if (iPut(server.iDBPath, vKey, sdslen(vKey), "str", 3, IDB_KEY_TYPE_NAME, server.iDBType) != 0) vResult = 0;
        }
        if (c->db->id != 0) sdsfree(vKey);
        if (vResult) {
            if (strncmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME))==0) {
                dictReplace(c->db->dict, key->ptr, value);
                incrRefCount(value);
            }
            addReply(c, shared.ok);
        }
        else
            addReplyError(c, "aset save disk error!");
    }
    else {
        addReplyError(c, "the idb is disabled, you should set idb-enabled to yes first on configuration first.");
    }
}
/* ADEL key attr */
void adelCommand(redisClient *c) {
    robj *key, *attr;
    int deleted = 0;
    if (server.iDBEnabled) {
        key   = c->argv[1];
        attr  = c->argv[2];

        sds vKey = getKeyNameOnIDB(c->db->id, key->ptr);
        char* vAttr = attr->ptr;
        if (vAttr) vAttr = vAttr[0] == '\0' ? IDB_VALUE_NAME : vAttr;

        if (strncasecmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME)) == 0) {
            if (dbDelete(c->db,key)) {
                signalModifiedKey(c->db,key);
                notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
                    "del",key,c->db->id);
                server.dirty++;
                deleted++;
            }
        }
        else
            if (iDelete(server.iDBPath, vKey, sdslen(vKey), attr->ptr, server.iDBType)) deleted++;
        if (c->db->id != 0) sdsfree(vKey);
        if (deleted) {
            addReply(c, shared.ok);
        }
        else
            addReplyError(c, "adel error!");
    }
    else {
        addReplyError(c, "the idb is disabled, you should set idb-enabled to yes first on configuration first.");
    }
}

/* AGET key attr */
void agetCommand(redisClient *c) {
    robj *key, *attr;
    sds value;
    if (server.iDBEnabled) {
        key   = c->argv[1];
        attr  = c->argv[2];

        char* vAttr = attr->ptr;
        if (vAttr) vAttr = vAttr[0] == '\0' ? IDB_VALUE_NAME : vAttr;
        if (strncasecmp(vAttr, IDB_VALUE_NAME, strlen(IDB_VALUE_NAME)) == 0) {
            getCommand(c);
        }
        else {
            sds vKey = getKeyNameOnIDB(c->db->id, key->ptr);
            value = iGet(server.iDBPath, vKey, sdslen(vKey), vAttr, server.iDBType);
            if (c->db->id != 0) sdsfree(vKey);
            key=createObject(REDIS_STRING, value);
            addReplyBulk(c, key);
            decrRefCount(key);
            //SDSFreeAndNil(value);
        }
    }
    else {
        addReplyError(c, "the idb is disabled, you should set idb-enabled to yes first on configuration first.");
    }
}

/* AEXISTS key attr */
void aexistsCommand(redisClient *c) {
    robj *key, *attr;
    if (server.iDBEnabled) {
        key   = c->argv[1];
        attr  = c->argv[2];
        sds vKey = getKeyNameOnIDB(c->db->id, key->ptr);
        char* vAttr = attr->ptr;
        if (vAttr) vAttr = vAttr[0] == '\0' ? IDB_VALUE_NAME : vAttr;
        if (iIsExists(server.iDBPath, vKey, sdslen(vKey), vAttr, server.iDBType)) {
            addReply(c, shared.cone);
        }
        else {
            addReply(c, shared.czero);
        }
        if (c->db->id != 0) sdsfree(vKey);
    }
    else {
        addReplyError(c, "the idb is disabled, you should set idb-enabled to yes first on configuration first.");
    }
}
