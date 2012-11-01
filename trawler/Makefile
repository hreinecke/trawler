#
#

PRG = trawler

SRCS = trawler.c watcher.c events.c sparse-file.c
OBJS = trawler.o watcher.o events.o sparse-file.o

CFLAGS = -Wall -g -D_GNU_SOURCE -I../include

all: $(PRG) mksparse

clean:
	rm -f $(OBJS)
	rm -f $(PRG)
	rm -f mksparse.o
	rm -f mksparse

mksparse: mksparse.o sparse-file.o
	$(CC) $(CFLAGS) -o $@ $^ ../lib/lib.a

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) ../lib/lib.a -lpthread

watcher.c: watcher.h
trawler.c: watcher.h events.h
events.c: events.h

