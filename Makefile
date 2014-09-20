SHELL = /bin/sh
CC = gcc
CFLAGS = -O -Wall -pedantic -Wno-variadic-macros -Wno-format -Wno-overlength-strings
LDFLAGS = -lpthread -lutil
TARGETS = demo tcp-client tcp-server udp-server udp-client

all: $(TARGETS)
tcp-client: tcp-client.o tcp-shared.o traffic-shared.o
tcp-server: tcp-server.o tcp-shared.o traffic-shared.o
udp-server: udp-server.o udp-shared.o traffic-shared.o
udp-client: udp-client.o udp-shared.o traffic-shared.o
tcp-client.o: tcp-client.c tcp-shared.h traffic-shared.h
tcp-server.o: tcp-server.c tcp-shared.h traffic-shared.h
udp-server.o: udp-server.c udp-shared.h traffic-shared.h
udp-client.o: udp-client.c udp-shared.h traffic-shared.h
traffic-shared.o: traffic-shared.c traffic-shared.h
tcp-shared.o: tcp-shared.c tcp-shared.h traffic-shared.h
udp-shared.o: udp-shared.c udp-shared.h traffic-shared.h
clean:
	rm -f *.o $(TARGETS)

