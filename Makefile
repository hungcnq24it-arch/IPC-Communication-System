
CC = gcc

CFLAGS = -Wall -Wextra -g
LIBS = -pthread -lrt

all: server client worker

server:
	$(CC) $(CFLAGS) $(LIBS) \
	src/socket/server.c -o server

client:
	$(CC) $(CFLAGS) \
	src/socket/client.c -o client

worker:
	$(CC) $(CFLAGS) $(LIBS) \
	src/message_queue/worker.c -o worker

clean:
	rm -f server client worker benchmark_app

.PHONY: all clean


benchmark:
	$(CC) $(CFLAGS) benchmark/benchmark.c -o benchmark_app

all: server client worker benchmark
