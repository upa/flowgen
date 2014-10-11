# Makefile

CC=gcc -g -Wall

DCE?=no
dce_yes=-fPIC
dce_no=

.c.o:
	$(CC) $(dce_$(DCE)) -c $< -o $@

all: flowgen

flowgen: flowgen.o
	$(CC) flowgen.o -o $@

clean:
	rm *.o
	rm flowgen
