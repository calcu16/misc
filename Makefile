# Student's Makefile for the CS:APP Data Lab
SHELL = /bin/sh
CC = gcc
CFLAGS = -O -Wall -pedantic -Wno-variadic-macros
LDFLAGS = -lpthread
TARGETS = traffic-client traffic-server

all: $(TARGETS)
traffic-client: traffic-client.o traffic-shared.o
traffic-server: traffic-server.o traffic-shared.o
traffic-client.o: traffic-client.c traffic-shared.h
traffic-server.o: traffic-server.c traffic-shared.h
traffic-shared.o: traffic-shared.c traffic-shared.h
clean:
	rm -f *.o $(TARGETS)

