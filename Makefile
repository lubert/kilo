# NOTES:
# $(CC) - make expands this to "cc"
# -Wall - all warnings
# -Wextra -pedantic - even more warnings
kilo: kilo.c
	$(CC) kilo.c -o kilo -Wall -Wextra -pedantic -std=c99
