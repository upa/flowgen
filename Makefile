# Makefile

CC=gcc -g -Wall

DCE?=no
dce_pic_yes=-fPIC
dce_pic_no=
dce_pie_yes=-pie
dce_pie_no=


CONNECT?=no
connect_yes=-DUDPCONNECT
connect_no=

.c.o:
	$(CC) $(dce_pic_$(DCE)) $(connect_$(CONNECT)) -DPOLL -c $< -o $@

all: flowgen

flowgen: flowgen.o Makefile
	$(CC) $(dce_pie_$(DCE)) flowgen.o -o $@ -lpthread

clean:
	rm *.o
	rm flowgen
