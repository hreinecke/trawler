#
#

PRG = dredger

SRCS = dredger.c watcher.c cli.c migrate.c backend-file.c
OBJS = dredger.o watcher.o cli.o migrate.o backend-file.o

CFLAGS = -Wall -g -D_GNU_SOURCE

all: $(PRG)

clean:
	rm -f $(OBJS)
	rm -f $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

dredger.c: list.h logging.h dredger.h cli.h fanotify-init-syscall.h
watcher.c: migrate.h fanotify.h fanotify-mark-syscall.h
migrate.c: logging.h migrate.h backend.h
cli.c: cli.h dredger.h
