mongocachestat
==============

[![Build Status](https://travis-ci.org/bwaldvogel/mongocachestat.png?branch=master)](https://travis-ci.org/bwaldvogel/mongocachestat)

Tool to collect block cache statistics for a MongoDB instance based on
[`$showDiskLoc`][showDiskLoc] and `mincore(2)`.

Prerequisites
-------------

- MongoDB 2.6 or newer because of [SERVER-5372][SERVER-5372]
- MongoDB client library and *headers* installed


Building
--------

    # apt-get install mongodb-dev libmongo-client-dev libboost-system-dev libboost-regex-dev libboost-thread-dev libboost-filesystem-dev
    # make

Usage
-----

MongoDB needs to be up and running.

    # ./mongocachestat [--directoryPerDb] <dbpath> [<host:port>]

Example:

    # ./mongocachestat /data/mongodb localhost:27017


Implementation detail
---------------------

Potential "Heisenberg effects" (i.e. changing the cache state by measuring with
this tool) are tried to minimized by using `madvise(…, …, MADV_RANDOM)`. It
turns off Linux kernel read-ahead and read-behind. We call it Heisenberg
compensator in analogy to a famous science fiction series.


[showDiskLoc]: http://docs.mongodb.org/manual/reference/operator/meta/showDiskLoc/
[SERVER-5372]: https://jira.mongodb.org/browse/SERVER-5372
