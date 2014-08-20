.PHONY: all clean

CFLAGS=-O2 -Wall -I. -std=c99

UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
TEST_CFLAGS := $(CFLAGS) -arch x86_64
CFLAGS := $(CFLAGS) -emit-llvm -arch x86_64
else
TEST_CFLAGS := $(CFLAGS)
endif

all: librope.a

clean:
	rm -f librope.a *.o tests

rope.o: rope.c rope.h
	$(CC) $(CFLAGS) -o rope.o $< -c

librope.a: rope.o
	ar rcs $@ $+

# Only need corefoundation to run the tests on mac
tests: librope.a test/tests.c test/benchmark.c test/slowstring.c
	$(CC) $(TEST_CFLAGS) $+ -o $@

