db.c -- redisDb operation functions
redis.h -- the redisDb struct defined here
config.c -- the new store-path parameter here

* redisClient
  * argc means arguments count
  * argv[] from 0 .. argc-1, 0 means self.
    * keys a?, argc[0]="keys", argc[1] = "a?"
* dict
  * dictsize(aDict) : get the count of keys in the dict

+ commands
  * subkeys keyPath pattern skipCount count


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
