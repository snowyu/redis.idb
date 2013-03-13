db.c -- redisDb operation functions
redis.h -- the redisDb struct defined here
config.c -- the new store-path parameter here

Options
-------

Only one database supports. the ‘select’ cmd is only for memory cache now.

* idb-enabled: enable/disable iDB storage. (iDBEnabled)
* idb-path: iDB store path. (iDBPath)
* idb-sync:  yes or no, write the iDB sync or async. (iDBSync)
* idb-pagesize: the max page size, pagesize <=0 means allow all keys list once (IDBMaxPageCount)
* rdb-enabled: enable/disable redis database dump file storage. (rdbEnabled)
* idb-type: iDB store type. (iDBType) deprecated.



Internal
---------


* !* renameGenericCommand should be optimal?

* idb-sync
  * dirtyKeys: dict
  * dirtyQueue: dict

* db.watched_keys: which clients is watching key.
    key -> clients
* redisClient
  * argc means arguments count
  * argv[] from 0 .. argc-1, 0 means self.
    * keys a?, argc[0]="keys", argc[1] = "a?"
* dict
  * dictsize(aDict) : get the count of keys in the dict

* + load/save expired info in iDB
* + remove expired keys in iDB 

* + commands
  * subkeys keyPath pattern skipCount count
* sdsnewlen(NULL, len):
  mine(idb) means: reserved free space len for NULL string.
  redis means: alloc a unknown string with length!
  now restore it, I added a new sdsalloc func for my requirement,
* redis data in iDB format(signalModifiedKey/lookupKey in db.c):
  * .type=redis
  * .value= 
        rio vRedisIO;
        rioInitWithBuffer(&vRedisIO,sdsempty());
        if (expired) {
            redisAssert(rdbSaveType(&vRedisIO,REDIS_RDB_OPCODE_EXPIRETIME_MS));
            redisAssert(rdbSaveMillisecondTime(&vRedisIO,vExpiredTime));
        }
        redisAssert(rdbSaveObjectType(&vRedisIO,val));
        redisAssert(rdbSaveObject(&vRedisIO,val));
        iPut(server.storePath, key->ptr, sdslen(key->ptr), vRedisIO.io.buffer.ptr, NULL, server.storeType);

the internal pubsub feature can notify key changed:

* notifyKeyspaceEvent(notify.c) 
  * howto get key do not triggle the events??? so I can use it as iDB save,
    * It seems that it would triggle events on read!!




Build
------

    git clone idb.c deps/idb
    make persist-settings
    make

One redis server can have many redisDb. from 0 to server.dbnum-1.

maybe I should add a new name defintion in redisDb:

the real storePath = server.storePath + '\' + redisDb.name

And I have to add a new command to set the redisDb name:

    use 1 as mydb_name

It do not necessary at all for iDB!!
