#!/bin/bash
export MYSQL_USER="root"
export MYSQL_PASSWORD="123456"
export MYSQL_DATABASE="ycwang"

./server -p 9006 "$@"
