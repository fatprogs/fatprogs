#
# Makefile for dosfstools (mkdosfs, dosfsck, dosfslabel and dosfsdump)
#

TOPDIR := $(shell pwd)

CC = gcc
CPP = $(CC) -E
DEFINES = -D_FILE_OFFSET_BITS=64 -DCONFIG_SYNC_FILE_RANGE
OPTFLAGS = -O2 -fomit-frame-pointer ${DEFINES}
WARNFLAGS = -Wall
DEBUGFLAGS =
CFLAGS = $(OPTFLAGS) $(WARNFLAGS) $(DEBUGFLAGS) -I$(TOPDIR)/include
LDFLAGS =

PREFIX =
SBINDIR = $(PREFIX)/sbin
MANDIR = $(PREFIX)/usr/man/man8

.PHONY: clean distclean install depend
.EXPORT_ALL_VARIABLES:

all dep clean install:
	$(MAKE) -C src $@

distclean:
	$(MAKE) -C src $@
	rm -f TAGS .#* .new* \#*# *~

debug: OPTFLAGS = -g -fno-omit-frame-pointer -DDEBUG -pg ${DEFINES}
debug: all

asan: OPTFLAGS = -g -fno-omit-frame-pointer -O0 -fsanitize=address ${DEFINES}
asan: LDFLAGS += -fsanitize=address
asan: all

TAGS:
	etags -d -T `find . -name '*.[ch]'`

dist: binary tar

tar: distclean
	cd ..; \
	name="$(notdir $(shell pwd))"; \
	namev="$$name-$(shell perl -ne 'print "$$1\n" if /VERSION.*"(\S+)"/;' version.h)"; \
	mv $$name $$namev; \
	tar cf $$namev.src.tar `find $$namev \( -name CVS -o -path $$namev/debian \) -prune -o ! -type d -print`; \
	gzip -9f $$namev.src.tar; \
	mv $$namev $$name

binary: all
	doit=""; [ root = "`whoami`" ] || doit=sudo; $$doit $(MAKE) binary-sub
	cd tmp; \
	name="$(notdir $(shell pwd))"; \
	namev="$$name-$(shell perl -ne 'print "$$1\n" if /VERSION.*"(\S+)"/;' version.h)"; \
	arch=`uname -m | sed 's/i.86/i386/'`; \
	nameva=$$namev.$$arch.tar; \
	tar cf ../../$$nameva * ; \
	gzip -9f ../../$$nameva
	doit=""; [ root = "`whoami`" ] || doit=sudo; $$doit rm -rf tmp

binary-sub:
	@[ root = "`whoami`" ] || (echo "Must be root for this!"; exit 1)
	mkdir -p tmp/$(SBINDIR) tmp/$(MANDIR)
	$(MAKE) install PREFIX=$(shell pwd)/tmp

# usage: make diff OLDVER=<last-release-number>
diff:
	@if [ "x$(OLDVER)" = "x" ]; then \
		echo "Usage: make diff OLDVER=<last-release-number>"; \
		exit 1; \
	fi; \
	name="$(notdir $(shell pwd))"; \
	namev="$$name-$(shell perl -ne 'print "$$1\n" if /VERSION.*"(\S+)"/;' version.h)"; \
	cvs diff -u -rRELEASE-$(OLDVER) >../$$namev.diff; \
	gzip -9f ../$$namev.diff


# usage: make release VER=<release-number>
release:
	@if [ "x$(VER)" = "x" ]; then \
		echo "Usage: make release VER=<release-number>"; \
		exit 1; \
	fi
	if [ -d CVS ]; then \
		modified=`cvs status 2>/dev/null | awk '/Status:/ { if ($$4 != "Up-to-date") print $$2 }'`; \
		if [ "x$$modified" != "x" ]; then \
			echo "There are modified files: $$modified"; \
			echo "Commit first"; \
			exit 1; \
		fi; \
	fi
	sed "/VERSION/s/\".*\"/\"$(VER)\"/" <version.h >version.h.tmp
	date="`date +'%d %b %Y'`"; sed "/VERSION_DATE/s/\".*\"/\"$$date\"/" <version.h.tmp >version.h
	rm version.h.tmp
	if [ -d CVS ]; then \
		cvs commit -m"Raised version to $(VER)" version.h; \
		cvs tag -c -F RELEASE-`echo $(VER) | sed 's/\./-/g'`; \
	fi

