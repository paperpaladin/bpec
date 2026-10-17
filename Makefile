CFLAGS = -std=c11 -g -O2 -Wall -Wextra -ffile-prefix-map=$(CURDIR)=.

bpec: bpec.c
	$(CC) $(CFLAGS) -o $@ bpec.c

test: bpec
	@t/run.sh

clean:
	rm -f bpec

.PHONY: test clean
