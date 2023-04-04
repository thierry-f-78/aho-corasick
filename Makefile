CFLAGS = -g -O3 -Wall

libaho-corasick.a: aho-corasick.o
	$(AR) rc libaho-corasick.a $^

test: libaho-corasick.a
	$(MAKE) -C test
	./test/test check test/data 2804

clean:
	rm -rf *.a *.o *.dSYM
	$(MAKE) -C test clean

.PHONY: test
