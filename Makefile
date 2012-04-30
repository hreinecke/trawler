#
#

SUBDIRS = trawler dredger

all: $(SUBDIRS)

clean:
	make -C trawler clean
	make -C dredger clean

trawler: FORCE
	make -C $@

dredger: FORCE
	make -C $@

FORCE:
