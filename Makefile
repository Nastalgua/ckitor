kilo: kilo.o append_buffer.o
	$(CC) kilo.o append_buffer.o -o kilo -Wall -Wextra -pedantic -std=c99

kilo.o: kilo.c
	$(CC) -c kilo.c -o kilo.o -Wall -Wextra -pedantic -std=c99

append_buffer.o: append_buffer.c
	$(CC) -c append_buffer.c -o append_buffer.o -Wall -Wextra -pedantic -std=c99

clean:
	rm -f *.o kilo
