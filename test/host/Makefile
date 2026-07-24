# Host-side unit tests for sota_signing. No ESP-IDF required.
# Run from the repo root:  make -C test/host test

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror
ROOT    := ../../simpleota
INC     := -I$(ROOT)/src -I$(ROOT)/src/vendor

OBJS := monocypher.o monocypher-ed25519.o sota_json.o sota_sha256.o sota_signing.o

test: test_signing
	./test_signing

test_signing: test_signing.c $(OBJS)
	$(CC) $(CFLAGS) $(INC) -o $@ test_signing.c $(OBJS)

monocypher.o: $(ROOT)/src/vendor/monocypher.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

monocypher-ed25519.o: $(ROOT)/src/vendor/monocypher-ed25519.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

sota_sha256.o: $(ROOT)/src/vendor/sota_sha256.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

sota_json.o: $(ROOT)/src/sota_json.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

sota_signing.o: $(ROOT)/src/sota_signing.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

clean:
	rm -f test_signing $(OBJS)

.PHONY: test clean
