CC = gcc
CFLAGS = -g -Wall -pthread

TARGETS = myshell server client demo

all: $(TARGETS)

myshell: main.c utils.c
	$(CC) $(CFLAGS) -o $@ main.c utils.c

# Server now includes scheduler.c
server: server.c utils.c net.c scheduler.c
	$(CC) $(CFLAGS) -o $@ server.c utils.c net.c scheduler.c

client: client.c net.c
	$(CC) $(CFLAGS) -o $@ client.c net.c

# The demo program
demo: demo.c
	$(CC) $(CFLAGS) -o $@ demo.c

clean:
	rm -f $(TARGETS) *.o *.log