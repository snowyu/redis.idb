Redis with iDB Storage
======================


The branch adds a new [iDB](https://github.com/snowyu/idb.c) Disk Data Storage to
redis. You can use the redis as database storage with iDB. And iDB can be used to
replace the original RDB Disk Data Storage too.

Disk Data Storage Features Comparison
-------------------------------------

### RDB Storage

Purpose: Redis is a memcache, but can save the memcache data to disk.

* All save to disk even if it is only a change occurs.
* Support Asynchronous saved only.
* much greater than the memory capacity of the storage will cause problems

### iDB Storage

Purpose: the Key/value Database(Persistent data storage) with a memcache.

* Save changes only, not all.
* Support Synchronous or Asynchronous saved.
* No problem with large data storage.
* Support hierarchy key/value, like path of a key.
* Support subkeys with pagination or not.
* Support the local partition keys for pagination.
* support the multiple attributes of a key
  * the key name can not include any dot '.' char
    * if u wana use the '.' char in key name, u must escape it by '\'
    * seperate the key and attribute by dot('.') char.
  * the attribute is always string.
  * the 'get key.value' is same as 'get key'
  * get/set key.attribute

if set the expired time, it will cache in memory only instead of saving to iDB.
if set the negated expired time, it will cache in memory for the abs(expired_time) and save to iDB.



Configuration
-------------

the new options added to redis.conf:

    # enable/disable iDB storage
    idb-enabled yes
    # the store path for iDB
    idb-path ./my.idb
    # the max page size for idb
    # 0 means do not enable the pagination(partition key).
    idb-pagesize 0
    # enable/disable sync write or async write on background.
    idb-sync no
    # enabled/disable the redis dump file(rdb) storage.
    rdb-enabled no

Donot forget these options:

    #maybe u wanna limit the memory cache size:
    maxmemory 512mb
    #it's a cache only now, so just remove any key accordingly to the LRU algorith
    maxmemory-policy allkeys-lru


The database 0 means default and root database in iDB.
The others database number can be visited in the default database(just keep compatibility):

    select 1
    set KeyName 1
    select 2
    set KeyName 2
    select 0
    get .db1/KeyName  #get KeyName in the database 1.
    get .db2/KeyName  #get KeyName in the database 2.

Commands
--------

New commands list here:

* subkeys [KeyPath [subkeysPattern [Count [skipCount]]]]
  * the subkeys will iterate all matched subkeys in the keyPath for iDB.
  * skipCount: skip the count of matched subkeys before.
  * count: return the count of matched subkeys. it should be less than or equ idb-pagesize(if idb-pagesize enabled).
  * eg, subkeys, will return all the keys in the iDB if idb-pagesize == 0.
  * Note: keys command only return all keys in the memory cache.
* Attribute operations: [todo these attributes are Disk IO directly, no memcache yet]
  * the attribute ".value" means the key's value.
  * the attribute ".type" means the key's type.
  * aset key attribute value: set the attribute of the key
  * aget/aexists/adel key attribute: get/(check exists)/del the attribute of the key
  * attrs key: list the attributes of the key(not fined)


Build
------

before building, you need ensure cmake installed, and run

    git submodule update --init --recursive


if you update the deps/idb/ by git pull, you should make idb too:

    cd deps/idb
    git checkout master
    git pull
    make

Todo
-----

* force to read the key's attributes from disk.
* the key's attributes should be cached in memory.
  * the simple way is treat the "." as reserved word, do not be allowed in key name.
* the current dbsize command only for the keys in memcache.
* the most of debug command only for the keys in memcache.
* ![Bug] AOF can not work on iDB. because AOF is still use dict(memcache) to save.
* !List, Set, Hash items should treat as subkey when saving to disk.
  * Or if items is very large to do so.
* !+ abstract new storage struct for memcache, aof, rdb, disk storage
* Exchange value should be as an atomic operation?
  * I use two dicts to keep the changed keys, one is dirtyKeys, another is dirtyQueue. (if asynchronous is enabled.)
  * modifies will be added to dirtyKeys if no saving.
  * any modifies will be added to dirtyQueue when bgsaving occurs.
  * the dirtyKeys will be empty and swap them(dirtyQueue, dirtyKeys) after saving
  * maybe it is no problem, redis is single thread app after all.

Redis
=====


Where to find complete Redis documentation?
-------------------------------------------
=======
This README is just a fast *quick start* document. You can find more detailed documentation at http://redis.io.

What is Redis?
--------------

Redis is often referred as a *data structures* server. What this means is that Redis provides access to mutable data structures via a set of commands, which are sent using a *server-client* model with TCP sockets and a simple protocol. So different processes can query and modify the same data structures in a shared way.

Data structures implemented into Redis have a few special properties:

* Redis cares to store them on disk, even if they are always served and modified into the server memory. This means that Redis is fast, but that is also non-volatile.
* Implementation of data structures stress on memory efficiency, so data structures inside Redis will likely use less memory compared to the same data structure modeled using an high level programming language.
* Redis offers a number of features that are natural to find in a database, like replication, tunable levels of durability, cluster, high availability.

Another good example is to think of Redis as a more complex version of memcached, where the operations are not just SETs and GETs, but operations to work with complex data types like Lists, Sets, ordered data structures, and so forth.

If you want to know more, this is a list of selected starting points:

* Introduction to Redis data types. http://redis.io/topics/data-types-intro
* Try Redis directly inside your browser. http://try.redis.io
* The full list of Redis commands. http://redis.io/commands
* There is much more inside the Redis official documentation. http://redis.io/documentation

Building Redis
--------------

Redis can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit
and 64 bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our
support for this platform is *best effort* and Redis is not guaranteed to
work as well as in Linux, OSX, and \*BSD there.

It is as simple as:

    % make

You can run a 32 bit Redis binary using:

    % make 32bit

After building Redis is a good idea to test it, using:

    % make test

Fixing build problems with dependencies or cached build options
---------

Redis has some dependencies which are included into the `deps` directory.
`make` does not rebuild dependencies automatically, even if something in the
source code of dependencies is changes.

When you update the source code with `git pull` or when code inside the
dependencies tree is modified in any other way, make sure to use the following
command in order to really clean everything and rebuild from scratch:

    make distclean

This will clean: jemalloc, lua, hiredis, linenoise.

Also if you force certain build options like 32bit target, no C compiler
optimizations (for debugging purposes), and other similar build time options,
those options are cached indefinitely until you issue a `make distclean`
command.

Fixing problems building 32 bit binaries
---------

If after building Redis with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a
`make distclean` in the root directory of the Redis distribution.

In case of build errors when trying to build a 32 bit binary of Redis, try
the following steps:

* Install the packages libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

Allocator
---------

Selecting a non-default memory allocator when building Redis is done by setting
the `MALLOC` environment variable. Redis is compiled and linked against libc
malloc by default, with the exception of jemalloc being the default on Linux
systems. This default was picked because jemalloc has proven to have fewer
fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

    % make MALLOC=libc

To compile against jemalloc on Mac OS X systems, use:

    % make MALLOC=jemalloc

Verbose build
-------------

Redis will build with a user friendly colorized output by default.
If you want to see a more verbose output use the following:

    % make V=1

Running Redis
-------------

To run Redis with the default configuration just type:

    % cd src
    % ./redis-server

If you want to provide your redis.conf, you have to run it using an additional
parameter (the path of the configuration file):

    % cd src
    % ./redis-server /path/to/redis.conf

It is possible to alter the Redis configuration passing parameters directly
as options using the command line. Examples:

    % ./redis-server --port 9999 --slaveof 127.0.0.1 6379
    % ./redis-server /etc/redis/6379.conf --loglevel debug

All the options in redis.conf are also supported as options using the command
line, with exactly the same name.

Playing with Redis
------------------

You can use redis-cli to play with Redis. Start a redis-server instance,
then in another terminal try the following:

    % cd src
    % ./redis-cli
    redis> ping
    PONG
    redis> set foo bar
    OK
    redis> get foo
    "bar"
    redis> incr mycounter
    (integer) 1
    redis> incr mycounter
    (integer) 2
    redis>

You can find the list of all the available commands at http://redis.io/commands.

Installing Redis
-----------------

In order to install Redis binaries into /usr/local/bin just use:

    % make install

You can use `make PREFIX=/some/other/directory install` if you wish to use a
different destination.

Make install will just install binaries in your system, but will not configure
init scripts and configuration files in the appropriate place. This is not
needed if you want just to play a bit with Redis, but if you are installing
it the proper way for a production system, we have a script doing this
for Ubuntu and Debian systems:

    % cd utils
    % ./install_server.sh

The script will ask you a few questions and will setup everything you need
to run Redis properly as a background daemon that will start again on
system reboots.

You'll be able to stop and start Redis using the script named
`/etc/init.d/redis_<portnumber>`, for instance `/etc/init.d/redis_6379`.

Code contributions
---

Note: by contributing code to the Redis project in any form, including sending
a pull request via Github, a code fragment or patch via private email or
public discussion groups, you agree to release your code under the terms
of the BSD license that you can find in the [COPYING][1] file included in the Redis
source distribution.

Please see the [CONTRIBUTING][2] file in this source distribution for more
information.

Enjoy!

[1]: https://github.com/antirez/redis/blob/unstable/COPYING
[2]: https://github.com/antirez/redis/blob/unstable/CONTRIBUTING
