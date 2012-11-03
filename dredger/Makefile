#
#

PRG = dredger
LIB = ../lib/lib.a
SRCS = dredger.c watcher.c cli-server.c migrate.c backend.c backend-file.c
OBJS = dredger.o watcher.o cli-server.o migrate.o backend.o backend-file.o

CFLAGS = -Wall -g -D_GNU_SOURCE -I../include

all: $(PRG)

clean:
	rm -f $(OBJS)
	rm -f $(PRG)

$(PRG): $(LIB) $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIB) -lpthread

dredger.c: fanotify.h fanotify-init-syscall.h backend.h watcher.h \
	cli-server.h
watcher.c: fanotify.h dredger.h backend.h
migrate.c: migrate.h backend.h fanotify.h fanotify-mark-syscall.h
backend.c: backend.h
cli-server.c: backend.h dredger.h migrate.h cli-server.h
