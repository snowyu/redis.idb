/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

#include <signal.h>
#include <ctype.h>
#include "../deps/idb/src/idb_helpers.h"

void slotToKeyAdd(robj *key);
void slotToKeyDel(robj *key);
void slotToKeyFlush(void);

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

static inline sds getKeyNameOnIDB(int dbId, sds key) {
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

static inline dictEntry *getDictEntryOnDirtyKeys(redisDb *db, robj *key)
{
    dict *d = db->dirtyQueue;
    dictEntry *de = dictFind(d, key);
    if (de) {
        //printf("%s found on dirtyQueue %d\n", key->ptr, dictGetVal(de) == NULL);
        return de;
    }
    d = db->dirtyKeys;
    de = dictFind(d, key);
    if (de) {
        //printf("%s found on dirtyKeys %d\n", key->ptr, dictGetVal(de) == NULL);
        return de;
    }
    return NULL;
}
/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
static int saveKeyValuePairOnIDB(redisDb *db, robj *key, robj *val)
{
    long long now = mstime();
    long long vExpiredTime = getExpire(db,key);
    int vExpired = (vExpiredTime != -1 && vExpiredTime < now);
    if (!vExpired) {
        rio vRedisIO;
        rioInitWithBuffer(&vRedisIO,sdsempty());
        vExpired = 1; //store the operation result, default is successful.
        if (vExpiredTime != -1) {
            if (rdbSaveType(&vRedisIO,REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1) vExpired = -1;
            if (rdbSaveMillisecondTime(&vRedisIO,vExpiredTime) == -1) vExpired = -1;
        }
        if (rdbSaveObjectType(&vRedisIO,val) == -1) vExpired = -1;
        if (rdbSaveObject(&vRedisIO,val) == -1) vExpired = -1;
        sds v = vRedisIO.io.buffer.ptr;
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        if (iPut(server.iDBPath, vKey, sdslen(vKey), v, sdslen(v), NULL, server.iDBType) != 0) vExpired = -1;
        if (iPut(server.iDBPath, vKey, sdslen(vKey), "redis", 5, ".type", server.iDBType) != 0) vExpired = -1;
        if (db->id != 0) sdsfree(vKey);
        sdsfree(v);
        return vExpired;
    }
    else
        return 0;
}

static void setKeyOnIDB(redisDb *db, robj *key) {
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
                    incrRefCount(val);
                    if (dictReplace(d, key, val) == 1) //new key
                        incrRefCount(key);
                }
            }
    }
}

static inline int deleteOnIDB(redisDb *db, robj *key) {
    int result = server.iDBEnabled;
    if (result) {
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        if (server.iDBSync) {
            //redisLog(REDIS_NOTICE, "Try deleteOnIDB:%s", vKey);
            result = iKeyIsExists(server.iDBPath, vKey, sdslen(vKey));
            if (result) {
                //redisLog(REDIS_NOTICE, "deleted:%s", vKey);
                iKeyDelete(server.iDBPath, vKey, sdslen(vKey));
            }
        }
        else {
            dict *d = server.idb_child_pid == -1 ? db->dirtyKeys : db->dirtyQueue;
            result = dictUpdate(d, key, NULL) != NULL;
            if (!result) { //not found in dirtyKeys
                d = (d == db->dirtyKeys) ? db->dirtyQueue: db->dirtyKeys;
                result = dictFind(d, key) != NULL || iKeyIsExists(server.iDBPath, vKey, sdslen(vKey));
                //if (result) { //in case the key is on writing...., so just added it.
                    incrRefCount(key);
                    dictAdd(d, key, NULL);
                //}
            }
            //dictEntry *de = dictFind(d, key);
            //redisAssertWithInfo(NULL, key, dictGetVal(de) == NULL);
            //redisLog(REDIS_NOTICE, "deleteOnIDB:%s", key->ptr);
            //server.dirty++;
        }

        if (db->id != 0) sdsfree(vKey);
    }
    return result;
}

int flushDictToIDB(redisDb *db, dict *d) {
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
            long long expire;
            if (o == NULL) {
                if (!iKeyDelete(server.iDBPath, key->ptr, sdslen(key->ptr))) {
                    vErr = REDIS_ERR;
                    break;
                };
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
        vErr = flushDictToIDB(db, d);
        if (vErr == REDIS_OK) {
            dictEmpty(d);
            d = db->dirtyQueue;
            count += dictSize(d);
            vErr = flushDictToIDB(db, d);
            if (vErr == REDIS_OK) dictEmpty(d);
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
        vErr = flushDictToIDB(db, d);
        if (vErr) break;
    }
    if (vErr == REDIS_OK) {
        redisLog(REDIS_NOTICE,"IDB saved on disk");
        server.idb_lastsave = time(NULL);
        server.idb_lastbgsave_status = REDIS_OK;
    }
    return vErr;
}

int iDBSaveBackground()
{
    pid_t childpid;
    long long start;

    if (server.idb_child_pid != -1) return REDIS_ERR;

    server.idb_dirty_before_bgsave = server.dirty;

    start = ustime();
    if ((childpid = fork()) == 0) {
        int retval;

        /* Child */
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);
        redisSetProcTitle("redis-idb-bgsave");
        retval = flushToIDB();
        if (retval == REDIS_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                redisLog(REDIS_NOTICE,
                    "IDB: %lu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
        }
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
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
        int j;
        for (j = 0; j < server.dbnum; j++) {
            redisDb *db = server.db+j;
            dict *d = db->dirtyKeys;
            dictEmpty(d);
            d = db->dirtyQueue;
            db->dirtyQueue = db->dirtyKeys;
            db->dirtyKeys = d;
        }
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "IDB Background saving error");
        server.idb_lastbgsave_status = REDIS_ERR;
    } else {
        redisLog(REDIS_WARNING,
            "IDB Background saving terminated by signal %d", bysignal);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        if (bysignal != SIGUSR1)
            server.idb_lastbgsave_status = REDIS_ERR;
    }
    server.idb_child_pid = -1;
    server.idb_save_time_last = time(NULL)-server.idb_save_time_start;
    server.idb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}

//lookup the key on IDB and cache it in the memory if found.
robj *lookupKeyOnIDB(redisDb *db, robj *key) {
    robj *result = NULL;
    //printf("lookupKeyOnIDB: %s\n", (char*)key->ptr);
    if (server.iDBEnabled) {
        if (server.iDBSync == 0) {
            //dict *d = server.idb_child_pid == -1 ? db->dirtyKeys : db->dirtyQueue;
            //if (server.idb_child_pid == -1)

            dictEntry *de = getDictEntryOnDirtyKeys(db, key);
            if (de) {
                result = dictGetVal(de);
                if (result) {
                    sds copy = sdsdup(key->ptr);
                    int retval = dictAdd(db->dict, copy, result);
                    redisAssertWithInfo(NULL,key,retval == REDIS_OK);
                }
                return result;
            }

        }
        sds vKey = getKeyNameOnIDB(db->id, key->ptr);
        //printf("try iGet:%s\n", vKey);
        sds vValueBuffer = iGet(server.iDBPath, vKey, sdslen(vKey), NULL, server.iDBType);
        //printf("lookupKeyOnIDB Loaded: %s=%s\n", (char*)vKey, vValueBuffer);
        if (db->id != 0) sdsfree(vKey);
        if (vValueBuffer) {
            rio vRedisIO;
            rioInitWithBuffer(&vRedisIO,vValueBuffer);
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
            //    addReplyErrorFormat(c,"invalid key type on %s", (char*)key->ptr);
            sdsfree(vValueBuffer);
        }
    }
    return result;
}

robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (server.rdb_child_pid == -1 && server.aof_child_pid == -1 && server.idb_child_pid == -1)
            val->lru = server.lruclock;
        return val;
    } else {
        return lookupKeyOnIDB(db, key);
    }
}

robj *lookupKeyRead(redisDb *db, robj *key) {
    robj *val;

    expireIfNeeded(db,key);
    val = lookupKey(db,key);
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    int retval = dictAdd(db->dict, copy, val);

    redisAssertWithInfo(NULL,key,retval == REDIS_OK);
    if (server.cluster_enabled) slotToKeyAdd(key);
    //retval = sdslen(val->ptr);
    //if (retval) iPut(server.iDBPath, key->ptr, sdslen(key->ptr), val->ptr, NULL, server.iDBType);
 }

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    struct dictEntry *de = dictFind(db->dict,key->ptr);

    redisAssertWithInfo(NULL,key,de != NULL);
    dictReplace(db->dict, key->ptr, val);
    //iPut(server.iDBPath, key->ptr, sdslen(key->ptr), val->ptr, NULL, server.iDBType);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent). */
void setKey(redisDb *db, robj *key, robj *val) {
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    removeExpire(db,key);
    signalModifiedKey(db,key);
}

int dbExists(redisDb *db, robj *key) {
    int result = dictFind(db->dict,key->ptr) != NULL;
    if (!result && server.iDBEnabled) {
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
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    struct dictEntry *de;

    while(1) {
        sds key;
        robj *keyobj;

        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    int result = dictDelete(db->dict,key->ptr) == DICT_OK;
    result = deleteOnIDB(db, key) || result;
    if (result)
        if (server.cluster_enabled) slotToKeyDel(key);
    return result;
}

long long emptyDb() {
    int j;
    long long removed = 0;
    //size_t count = 0;

    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);
        dictEmpty(server.db[j].dict);
        dictEmpty(server.db[j].expires);
        if (server.iDBEnabled) {
            //count += dictSize(server.db[j].dirtyKeys);
            //count += dictSize(server.db[j].dirtyQueue);
            dictEmpty(server.db[j].dirtyKeys);
            dictEmpty(server.db[j].dirtyQueue);
        }
    }
    if (server.iDBEnabled) {
        //redisLog(REDIS_NOTICE, "emptyDb dirtyKeys:%lu", count);
        iKeyDelete(server.iDBPath, NULL, 0);
    }
    return removed;
}

int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/
void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    setKeyOnIDB(db, key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

//flush means emptyDB!!!

void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    signalFlushedDb(c->db->id);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    if (server.cluster_enabled) slotToKeyFlush();
    if (server.iDBEnabled) {
        if (server.idb_child_pid != -1) {
            kill(server.idb_child_pid,SIGUSR1);
        }
        size_t count = 0;
        count += dictSize(c->db->dirtyKeys);
        count += dictSize(c->db->dirtyQueue);
        dictEmpty(c->db->dirtyKeys);
        dictEmpty(c->db->dirtyQueue);
        sds vKey = getKeyNameOnIDB(c->db->id, NULL);
        int result = iKeyDelete(server.iDBPath, vKey, sdslen(vKey));
        redisLog(REDIS_NOTICE, "DeleteDB(%lu) on %s is %d pid=%d", count, vKey, result, server.idb_child_pid);
        if (c->db->id != 0) sdsfree(vKey);
    }
    addReply(c,shared.ok);
}

void flushallCommand(redisClient *c) {
    signalFlushedDb(-1);
    server.dirty += emptyDb();
    if (server.cluster_enabled) slotToKeyFlush();
    addReply(c,shared.ok);
    if (server.rdb_child_pid != -1) {
        kill(server.rdb_child_pid,SIGUSR1);
        rdbRemoveTempFile(server.rdb_child_pid);
    }

    if (server.iDBEnabled) {
        if (server.idb_child_pid != -1)
            kill(server.idb_child_pid,SIGUSR1);
        iKeyDelete(server.iDBPath, NULL, 0);
        //if (flushAllToIDB() != REDIS_OK)
        //    addReplyError(c, "Flush all on iDB Error");
    }

    if (server.saveparamslen > 0 && server.rdbEnabled) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = server.dirty;
        rdbSave(server.rdb_filename);
        server.dirty = saved_dirty;
    }
    server.dirty++;
}

void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (dbDelete(c->db,c->argv[j])) {
            signalModifiedKey(c->db,c->argv[j]);
            notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);
}

void existsCommand(redisClient *c) {
    expireIfNeeded(c->db,c->argv[1]);
    if (dbExists(c->db,c->argv[1])) {
        addReply(c, shared.cone);
    } else {
        addReply(c, shared.czero);
    }
}

void selectCommand(redisClient *c) {
    long id;

    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != REDIS_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == REDIS_ERR) {
        addReplyError(c,"invalid DB index");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(redisClient *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c);

    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredMultiBulkLength(c,replylen,numkeys);
}

void subkeysCommand(redisClient *c) {
    if (server.iDBEnabled) {
        sds vKeyPath = c->argv[1]->ptr;
        sds vPattern = c->argv[2]->ptr;
        long vSkipCount, vCount;
        if (getLongFromObjectOrReply(c, c->argv[3], &vSkipCount, NULL) != REDIS_OK) return;
        if (getLongFromObjectOrReply(c, c->argv[4], &vCount, NULL) != REDIS_OK) return;
        vCount = (IDBMaxPageCount > 0 && vCount > IDBMaxPageCount) ? IDBMaxPageCount : vCount;

        unsigned long numkeys = 0;
        void *replylen = addDeferredMultiBulkLength(c);

        vPattern = (vPattern[0] == '*' && vPattern[1] == '\0') || vPattern[0] == '\0' ? NULL : vPattern;
        dStringArray *vResult = iSubkeys(server.iDBPath, vKeyPath, sdslen(vKeyPath), vPattern,
            vSkipCount, vCount);
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

void dbsizeCommand(redisClient *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(redisClient *c) {
    addReplyLongLong(c,server.lastsave);
}

void typeCommand(redisClient *c) {
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "string"; break;
        case REDIS_LIST: type = "list"; break;
        case REDIS_SET: type = "set"; break;
        case REDIS_ZSET: type = "zset"; break;
        case REDIS_HASH: type = "hash"; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);
}

void shutdownCommand(redisClient *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= REDIS_SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= REDIS_SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    if (prepareForShutdown(flags) == REDIS_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(redisClient *c, int nx) {
    robj *o;
    long long expire;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]);
    }
    dbAdd(c->db,c->argv[2],o);
    if (expire != -1) setExpire(c->db,c->argv[2],expire);
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}

void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;

    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }

    /* Return zero if the key already exists in the target DB */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    dbDelete(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

void setExpire(redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    redisAssertWithInfo(NULL,key,kde != NULL);
    de = dictReplaceRaw(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key) {
    robj *argv[2];

    argv[0] = shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);
    replicationFeedSlaves(server.slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

int expireIfNeeded(redisDb *db, robj *key) {
    long long when = getExpire(db,key);

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller, 
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (server.masterhost != NULL) {
        return mstime() > when;
    }

    /* Return when this key has not expired */
    if (mstime() <= when) return 0;

    /* Delete the key */
    server.stat_expiredkeys++;
    propagateExpire(db,key);
    notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED,
        "expired",key,db->id);
    return dbDelete(db,key);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */
void expireGenericCommand(redisClient *c, long long basetime, int unit) {
    dictEntry *de;
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
        return;

    if (unit == UNIT_SECONDS) when *= 1000;
    when += basetime;

    de = dictFind(c->db->dict,key->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we take the other branch of the IF statement setting an expire
     * (possibly in the past) and wait for an explicit DEL from the master. */
    if (when <= mstime() && !server.loading && !server.masterhost) {
        robj *aux;

        redisAssertWithInfo(c,key,dbDelete(c->db,key));
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL. */
        aux = createStringObject("DEL",3);
        rewriteClientCommandVector(c,2,aux,key);
        decrRefCount(aux);
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",key,c->db->id);
        addReply(c, shared.cone);
        return;
    } else {
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"expire",key,c->db->id);
        server.dirty++;
        return;
    }
}

void expireCommand(redisClient *c) {
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

void expireatCommand(redisClient *c) {
    expireGenericCommand(c,0,UNIT_SECONDS);
}

void pexpireCommand(redisClient *c) {
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

void pexpireatCommand(redisClient *c) {
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

void ttlGenericCommand(redisClient *c, int output_ms) {
    long long expire, ttl = -1;

    expire = getExpire(c->db,c->argv[1]);
    /* If the key does not exist at all, return -2 */
    if (expire == -1 && lookupKeyRead(c->db,c->argv[1]) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }
    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    if (expire != -1) {
        ttl = expire-mstime();
        if (ttl < 0) ttl = -1;
    }
    if (ttl == -1) {
        addReplyLongLong(c,-1);
    } else {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
    }
}

void ttlCommand(redisClient *c) {
    ttlGenericCommand(c, 0);
}

void pttlCommand(redisClient *c) {
    ttlGenericCommand(c, 1);
}

void persistCommand(redisClient *c) {
    dictEntry *de;

    de = dictFind(c->db->dict,c->argv[1]->ptr);
    if (de == NULL) {
        addReply(c,shared.czero);
    } else {
        if (removeExpire(c->db,c->argv[1])) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    }
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    REDIS_NOTUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }
    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        redisAssert(j < argc);
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

int *getKeysFromCommand(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
    if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys,flags);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

void getKeysFreeResult(int *result) {
    zfree(result);
}

int *noPreloadGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
    if (flags & REDIS_GETKEYS_PRELOAD) {
        *numkeys = 0;
        return NULL;
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

int *renameGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
    if (flags & REDIS_GETKEYS_PRELOAD) {
        int *keys = zmalloc(sizeof(int));
        *numkeys = 1;
        keys[0] = 1;
        return keys;
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
    int i, num, *keys;
    REDIS_NOTUSED(cmd);
    REDIS_NOTUSED(flags);

    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }
    keys = zmalloc(sizeof(int)*num);
    for (i = 0; i < num; i++) keys[i] = 3+i;
    *numkeys = num;
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster. */
void slotToKeyAdd(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    zslInsert(server.cluster->slots_to_keys,hashslot,key);
    incrRefCount(key);
}

void slotToKeyDel(robj *key) {
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    zslDelete(server.cluster->slots_to_keys,hashslot,key);
}

void slotToKeyFlush(void) {
    zslFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = zslCreate();
}

unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;
    
    n = zslFirstInRange(server.cluster->slots_to_keys, range);
    while(n && n->score == hashslot && count--) {
        keys[j++] = n->obj;
        n = n->level[0].forward;
    }
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    zskiplist *zsl = server.cluster->slots_to_keys;
    zskiplistNode *zn;
    zrangespec range;
    int rank, count = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    /* Find first element in range */
    zn = zslFirstInRange(zsl, range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn != NULL) {
        rank = zslGetRank(zsl, zn->score, zn->obj);
        count = (zsl->length - (rank - 1));

        /* Find last element in range */
        zn = zslLastInRange(zsl, range);

        /* Use rank of last element, if any, to determine the actual count */
        if (zn != NULL) {
            rank = zslGetRank(zsl, zn->score, zn->obj);
            count -= (zsl->length - rank);
        }
    }
    return count;
}
