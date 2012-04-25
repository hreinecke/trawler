#
#

PRG = trawler

SRCS = trawler.c watcher.c
OBJS = trawler.o watcher.o

CFLAGS = -Wall -g

all: $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

watcher.c: watcher.h
trawler.c: list.h watcher.h



