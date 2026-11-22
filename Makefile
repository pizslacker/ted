CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99

editor: ted.c
	$(CC) $(CFLAGS) ted.c -o ted

clean:
	rm -f editor