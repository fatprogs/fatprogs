
COMMON_OBJECTS = common.o
FSCK_OBJECTS = boot.o check.o fat.o file.o io.o lfn.o

all: dosfsck dosfslabel mkdosfs dosfsdump

dosfsck: $(COMMON_OBJECTS) $(FSCK_OBJECTS) dosfsck.o
	$(CC) -o $@ $(LDFLAGS) $^

dosfslabel: $(COMMON_OBJECTS) $(FSCK_OBJECTS) dosfslabel.o
	$(CC) -o $@ $(LDFLAGS) $^

dosfsdump: $(COMMON_OBJECTS) dosfsdump.o
	$(CC) -o $@ $(LDFLAGS) $^

mkdosfs: $(COMMON_OBJECTS) mkdosfs.o
	$(CC) -o $@ $(LDFLAGS) $^

.c.o:
	$(CC) -c $(CFLAGS) $*.c

install: dosfsck mkdosfs
	mkdir -p $(SBINDIR) $(MANDIR)
	install -m 755 dosfsck $(SBINDIR)
	install -m 644 dosfsck.8 $(MANDIR)
	install -m 755 mkdosfs $(SBINDIR)
	install -m 644 mkdosfs.8 $(MANDIR)
	install -m 755 dosfslabel $(SBINDIR)
	install -m 755 dosfsdump $(SBINDIR)
	rm -f $(SBINDIR)/fsck.msdos
	rm -f $(SBINDIR)/fsck.vfat
	ln -s dosfsck $(SBINDIR)/fsck.msdos
	ln -s dosfsck $(SBINDIR)/fsck.vfat
	rm -f $(MANDIR)/fsck.msdos.8
	ln -s dosfsck.8 $(MANDIR)/fsck.msdos.8
	ln -s dosfsck.8 $(MANDIR)/fsck.vfat.8
	rm -f $(SBINDIR)/mkfs.msdos
	rm -f $(SBINDIR)/mkfs.vfat
	ln -s mkdosfs $(SBINDIR)/mkfs.msdos
	ln -s mkdosfs $(SBINDIR)/mkfs.vfat
	rm -f $(MANDIR)/mkfs.msdos.8
	ln -s mkdosfs.8 $(MANDIR)/mkfs.msdos.8
	ln -s mkdosfs.8 $(MANDIR)/mkfs.vfat.8

clean:
	rm -f *.o *.s *.i *~ \#*# tmp_make .#* .new*

distclean: clean
	rm -f *.a dosfsck dosfslabel dosfsdump mkdosfs

dep:
	sed '/\#\#\# Dependencies/q' <Makefile >tmp_make
	$(CPP) $(CFLAGS) -MM *.c >>tmp_make
	mv tmp_make Makefile
