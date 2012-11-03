#
#

SUBDIRS = lib trawler dredger

all: $(SUBDIRS)

clean:
	make -C lib clean
	make -C trawler clean
	make -C dredger clean

$(SUBDIRS): FORCE
	make -C $@

FORCE:
