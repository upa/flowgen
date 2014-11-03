# Makefile

CC=gcc -g -Wall

DCE?=no
dce_pic_yes=-fPIC
dce_pic_no=
dce_pie_yes=-pie
dce_pie_no=

.c.o:
	$(CC) $(dce_pic_$(DCE)) -c $< -o $@

all: flowgen

flowgen: flowgen.o
	$(CC) $(dce_pie_$(DCE)) flowgen.o -o $@ -lpthread

clean:
	rm *.o
	rm flowgen
