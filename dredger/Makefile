#
#

PRG = dredger

SRCS = dredger.c watcher.c cli.c
OBJS = dredger.o watcher.o cli.o

CFLAGS = -Wall -g -D_GNU_SOURCE

all: $(PRG)

clean:
	rm -f $(OBJS)
	rm -f $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

watcher.c: fanotify.h fanotify-syscall.h
cli.c: cli.h
