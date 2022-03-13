
WARN_CFLAGS =  -Wall -Wformat -Wformat-security -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wextra -Wshadow -Wcast-align -Wwrite-strings -Waggregate-return -Wstrict-prototypes -Winline -Wredundant-decls -Wno-sign-compare -fexceptions -fasynchronous-unwind-tables
CFLAGS=-I /usr/include/libxml2 -g3 -Wall $(WARN_CFLAGS)
LDFLAGS=-laugeas
LD_LIBRARY_PATH=/lib:/usr/lib

augsuggest	:	augsuggest.c augsuggest.h

