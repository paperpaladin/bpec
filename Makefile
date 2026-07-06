CFLAGS = -std=c11 -g -O2 -Wall -Wextra -ffile-prefix-map=$(CURDIR)=.

bpec: bpec.c
	$(CC) $(CFLAGS) -o $@ bpec.c

test: bpec
	@t/run.sh

# Input here is untrusted text and the byte-level map is defined for all
# 256 byte values, so there is no input bpec may refuse and none it may
# crash on. ASan is the oracle for that half: the suite cannot tell a
# correct answer from a lucky read past the end of a buffer, and it is
# the hostile sweep in particular that this target exists for.
test-asan:
	$(CC) -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -ffile-prefix-map=$(CURDIR)=. \
	      -o bpec-asan bpec.c
	@BPEC=./bpec-asan BPEC_RANDOM=200 BPEC_CORPUS_BYTES=400000 t/run.sh

clean:
	rm -f bpec bpec-asan

.PHONY: test test-asan clean
