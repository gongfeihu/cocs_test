fastcdc: fastcdc.o
	gcc fastcdc.o -o fastcdc -lssl -lcrypto

fastcdc.o: fastcdc.c fastcdc.h
	gcc -c -g fastcdc.c

clean:
	rm -f fastcdc fastcdc.o

.PHONY: clean