#
#

PRG = trawler

SRCS = trawler.c watcher.c events.c
OBJS = trawler.o watcher.o events.o

CFLAGS = -Wall -g -D_GNU_SOURCE -I../include

all: $(PRG) mksparse

clean:
	rm -f $(OBJS)
	rm -f $(PRG)
	rm -f mksparse.o
	rm -f mksparse

mksparse: mksparse.o
	$(CC) $(CFLAGS) -o $@ $<

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

watcher.c: watcher.h
trawler.c: watcher.h events.h
events.c: list.h events.h

