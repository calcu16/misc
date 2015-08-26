#!/bin/env python
from tempfile import mkdtemp
import os
import re
import sys
import subprocess
import shutil
from glob import glob

class IgnorableError(Exception):
  def __init__(self, value):
    self.value = value
  def __str__(self):
    return repr(self.value)

known_ok = re.compile('|'.join([
  r'''LaTeX Warning: Label\(s\) may have changed. Rerun to get cross-references right\.''',
  r'''LaTeX Warning: (?:Citation|Reference) `[^']+' on page (?:\d+|[\\]thepage ) undefined''',
  r'''LaTeX Warning: There were undefined references.'''
]))

def readLogfile(filename):
  OK    = object()
  ERROR = object()
  EXPLA = object()
  ERRORED = False
  RERUN   = False
  last = ""
  state = OK

  with open(filename) as logfile:
    for line in logfile:
      line = line.strip()
      RERUN |= line == "LaTeX Warning: Label(s) may have changed. Rerun to get cross-references right."
      if state is OK and line.startswith("!"):
        print(last)
        state = ERROR
        ERRORED = True
      if state in [ERROR, EXPLA] or line.startswith('LaTeX Warning:') and not known_ok.match(line):
        print(line)
        if not line:
          state = OK if state is EXPLA else EXPLA
      last = line
  return not RERUN or ERRORED


def compile(input, output, bibtex):
  tmp = mkdtemp(suffix="tex")
  for filename in glob('cls/*'):
    shutil.copy2(filename, tmp)
  try:
    while True:
      proc = subprocess.Popen(["pdflatex", "-interaction", "batchmode", "-output-directory=" + tmp, input], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
      proc.communicate()
      outfile = "%s.pdf" % os.path.splitext(os.path.basename(input))[0]
      logfile = "%s.log" % os.path.splitext(os.path.basename(input))[0]
      if not bibtex and readLogfile(os.path.join(tmp, logfile)):
        break
      if bibtex:
        bibdir, bibfile = os.path.split(bibtex)
        bibfile, _ = os.path.splitext(bibfile)
        bibout = os.path.join(tmp, bibdir)
        cwd = os.getcwd()
        try:
          shutil.copytree(bibdir, bibout)
          os.chdir(tmp)
          paper = os.path.splitext(os.path.split(input)[1])[0]
          (out, _) = subprocess.Popen(["bibtex", "--terse", paper]).communicate()
          bibtex = None
        finally:
          os.chdir(cwd)
    try:
      shutil.copy2(os.path.join(tmp, outfile), output)
    except IOError as e:
      raise IgnorableError(e)
  finally:
    shutil.rmtree(tmp)
    pass


if __name__ == "__main__":
  cwd = os.getcwd()
  input = os.path.abspath(sys.argv[1])
  output = os.path.abspath(sys.argv[2])
  bibtex = sys.argv[3] if len(sys.argv) > 3 else None
  try:
    compile(input, output, bibtex)
  except IgnorableError:
    pass
