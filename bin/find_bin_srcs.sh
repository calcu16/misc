#!/bin/sh
if [ -d "$*" ]; then
  find $* -name *.c |\
  xargs grep -c '^#define MAIN' /dev/null |\
  sed -n "s,:[^0]$,,p"
fi
