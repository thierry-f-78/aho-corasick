LDFLAGS = -g
CFLAGS = -g -O3 -Wall -I..
LDLIBS = -L.. -laho-corasick

test: test.o

../libaho-corasick.a:
	$(MAKE) -C ..

test.o: ../libaho-corasick.a

out.pdf: test
	./test dot data out.dot
	dot -Tpdf -o out.pdf out.dot

clean:
	rm -rf *.o *.dSYM test

.PHONY: out.pdf
