db.c -- redisDb operation functions
redis.h -- the redisDb struct defined here

One redis server can have many redisDb. from 0 to server.dbnum-1.

maybe I should add a new name defintion in redisDb:

the real storePath = server.storePath + '\' + redisDb.name

And I have to add a new command to set the redisDb name:

    use 1 as mydb_name

It do not necessary at all for iDB!!
