#/bin/env python3
from __future__ import print_function
from sys import stdin
for line in stdin:
  if line[-2] == '\\':
    print(line[:-2], end='')
  else:
   print(line, end='')
