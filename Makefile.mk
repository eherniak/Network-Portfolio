CC = gcc
CFLAGS = -Wall -Wextra -pthread
LDFLAGS = -lcrypto

all: proxy

proxy: proxy.c
	$(CC) $(CFLAGS) -o proxy proxy.c $(LDFLAGS)

clean:
	rm -f proxy
	rm -rf cache

.PHONY: all clean