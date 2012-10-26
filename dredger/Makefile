#
#

PRG = dredger

SRCS = dredger.c watcher.c cli.c migrate.c backend.c backend-file.c
OBJS = dredger.o watcher.o cli.o migrate.o backend.o backend-file.o

CFLAGS = -Wall -g -D_GNU_SOURCE -I../include

all: $(PRG)

clean:
	rm -f $(OBJS)
	rm -f $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lpthread

dredger.c: fanotify.h fanotify-init-syscall.h logging.h backend.h \
	watcher.h cli.h
watcher.c: fanotify.h fanotify-mark-syscall.h list.h logging.h \
	dredger.h backend.h
migrate.c: logging.h migrate.h backend.h
backend.c: backend.h
cli.c: logging.h backend.h dredger.h migrate.h cli.h
