#
#
#

LIB = lib.a
SRCS = cli.c logging.c
OBJS = cli.o logging.o

CFLAGS = -Wall -g -D_GNU_SOURCE -I../include

all: $(LIB)

$(LIB): $(OBJS)
	ar rcv $(LIB) $^

clean:
	rm -f $(LIB)
	rm -f $(OBJS)
