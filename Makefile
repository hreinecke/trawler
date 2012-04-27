#
#

PRG = trawler

SRCS = trawler.c watcher.c events.c
OBJS = trawler.o watcher.o events.o

DAEMON = dredger

DAEMON_SRCS = dredger.c
DAEMON_OBJS = dredger.o

CFLAGS = -Wall -g

all: $(PRG) $(DAEMON)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

$(DAEMON): $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_OBJS) -lpthread

watcher.c: watcher.h
trawler.c: watcher.h events.h
events.c: list.h events.h
dredger.c: fanotify.h fanotify-syscall.h

