#!/bin/env python3
from sys import argv, stdin
from os.path import isfile
(_, src, out, srcdir, objdir, bindir) = argv

def filename(dep):
  return dep[len(srcdir)+1:-2]

odeps = {}

for line in stdin:
  line = line.strip()
  (head, deps) = line.split(':', 1)
  deps = deps.split(' ')
  objdeps = [dep for dep in deps if isfile("%s/%s.c" % (srcdir, filename(dep)))]
  odeps[filename(objdeps[0])] = set(objdeps)

seen = set()

while odeps[src] - seen:
  dep = next(iter(odeps[src] - seen))
  seen.add(dep)
  odeps[src] |= odeps[filename(dep)]

print("%s: %s" % (out, " ".join(odeps[src])))
print("%s/%s: %s" % (bindir, src, " ".join(set("%s/%s.o" % (objdir, filename(o)) for o in odeps[src]))))

