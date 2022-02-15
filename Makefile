
CFLAGS=-I /usr/include/libxml2 -g3 -Wall
LDFLAGS=-laugeas
LD_LIBRARY_PATH=/lib:/usr/lib

augsuggest	:	augsuggest.c augsuggest.h

