#
#

PRG = trawler

SRCS = trawler.c watcher.c events.c
OBJS = trawler.o watcher.o events.o

CFLAGS = -Wall -g

all: $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

watcher.c: watcher.h
trawler.c: watcher.h events.h
events.c: list.h events.h


