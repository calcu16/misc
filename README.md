Misc
====
Miscellaneous scripts and programs

Traffic
===

Runs traffic between a server and client

To run:
  make
  ./traffic-server 9618
  ./traffic-client localhost 9618 1000 1024 1024 0

To capture time deltas use
  ./traffic-client -q localhost 9618 1000 1024 1024 0 | awk -F' ' '{print $7 - $6 + $15, $11 - $10 - $14}'

