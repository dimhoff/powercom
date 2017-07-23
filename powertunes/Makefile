all: powertunes

powertunes: powertunes.c
	gcc -g -std=c11 -g -Wall -o $@ $^ -lrt -lpthread

clean:
	rm -f powertunes
