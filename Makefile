CC       = gcc
CFLAGS   = -Wall -Wextra -pthread -g
TARGETS  = server http_server identity_server cliente

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o server server.c

http_server: http_server.c
	$(CC) $(CFLAGS) -o http_server http_server.c

identity_server: identity_server.c
	$(CC) $(CFLAGS) -o identity_server identity_server.c

cliente: cliente.c
	$(CC) $(CFLAGS) -o cliente cliente.c

clean:
	rm -f $(TARGETS)

run-game: server
	./server 8080 logs.txt

run-http: http_server
	./http_server 8081 http_logs.txt
