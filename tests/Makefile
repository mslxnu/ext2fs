CC=     cc
CFLAGS= -Wall -Wextra -std=c99 -fPIE -g

PROGS=  test_mount \
        test_readdir

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean
