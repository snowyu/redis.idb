#!/usr/bin/env bash

pids=`mktemp killredis.XXXXX`

#ps ax|grep redis-server|tr -s " "|cut -d " " -f 1 > $pids
ps ax|grep redis-server|awk '{print "kill -9 ", $1}' > $pids
ps ax|grep redis-idb-bgsave|awk '{print "kill -9 ", $1}' >> $pids
bash $pids
rm $pids
