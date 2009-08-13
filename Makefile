CC=gcc
LD=gcc
LDFLAGS=
CFLAGS=-g
CPPFLAGS=
LIBS=-lpthread

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: test.o syslog.o
	$(LD) $(LDFLAGS) $(LIBS) -o $@ $^


clean:
	rm -f test
	rm -f *.o

.PHONY: clean
