#!/bin/sh
if [ -d "$*" ]; then
  find $* -name *.c
fi
