Misc
====
Miscellaneous scripts and programs

Demo
===
Runs a program and supplies input from another file whenever an empty line is given from stdin

Traffic
===

Runs traffic between a server and client

To run:
  make
  ./tcp-server 9618
  ./tcp-client localhost 9618 1000 1024 1024 0

To capture time deltas use
  ./tcp-client -q localhost 9618 1000 1024 1024 0 | awk -F' ' '{print $7 - $6 + $15, $11 - $10 - $14}'

