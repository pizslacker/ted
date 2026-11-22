CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99

ted: ted.c
	$(CC) $(CFLAGS) ted.c -o ted

clean:
	rm -f ted