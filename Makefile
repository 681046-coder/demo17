CC ?= cc

inventory: server.c
	$(CC) -O2 -Wall -o inventory server.c

run: inventory
	./inventory 8080

clean:
	rm -f inventory
