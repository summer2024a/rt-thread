CC ?= gcc
RPSH_APP=rpsh_client

CFLAGS = -g -fPIC

all: ${RPSH_APP}

${RPSH_APP}: ${RPSH_APP}.o
	$(CC) -g -o $@ ${RPSH_APP}.o -lpthread -Wall

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf ${RPSH_APP} *.o