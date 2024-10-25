# Makefile for lab 7, part 2

CC      = gcc
CFLAGS  = -g -Wall

http-server: http-server.o

http-server.o: http-server.c

.PHONY: clean
clean:
	rm -f *.o a.out http-server

.PHONY: all
all: clean http-server
