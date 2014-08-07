# Student's Makefile for the CS:APP Data Lab
SHELL = /bin/sh
CC = gcc
CFLAGS = -O -Wall -pedantic -Wno-variadic-macros -Wno-format -Wno-overlength-strings
LDFLAGS = -lpthread
TARGETS = tcp-client tcp-serve demo

all: $(TARGETS)
tcp-client: tcp-client.o traffic-shared.o
tcp-server: tcp-server.o traffic-shared.o
tcp-client.o: tcp-client.c traffic-shared.h
tcp-server.o: tcp-server.c traffic-shared.h
traffic-shared.o: traffic-shared.c traffic-shared.h
clean:
	rm -f *.o $(TARGETS)

