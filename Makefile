#
#

PRG = trawler

SRCS = trawler.c
OBJS = trawler.o

CFLAGS = -Wall -g

all: $(PRG)

$(PRG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)


