
OBJECTS = boot.o check.o common.o fat.o file.o io.o lfn.o

all: dosfsck dosfslabel

dosfsck: $(OBJECTS) dosfsck.o
	$(CC) -o $@ $(LDFLAGS) $^

dosfslabel : $(OBJECTS) dosfslabel.o
	$(CC) -o $@ $(LDFLAGS) $^

.c.o:
	$(CC) -c $(CFLAGS) $*.c

install: dosfsck
	mkdir -p $(SBINDIR) $(MANDIR)
	install -m 755 dosfsck $(SBINDIR)
	install -m 644 dosfsck.8 $(MANDIR)
	install -m 755 dosfslabel $(SBINDIR)
	rm -f $(SBINDIR)/fsck.msdos
	rm -f $(SBINDIR)/fsck.vfat
	ln -s dosfsck $(SBINDIR)/fsck.msdos
	ln -s dosfsck $(SBINDIR)/fsck.vfat
	rm -f $(MANDIR)/fsck.msdos.8
	ln -s dosfsck.8 $(MANDIR)/fsck.msdos.8
	ln -s dosfsck.8 $(MANDIR)/fsck.vfat.8

clean:
	rm -f *.o *.s *.i *~ \#*# tmp_make .#* .new*

distclean: clean
	rm -f *.a dosfsck dosfslabel

dep:
	sed '/\#\#\# Dependencies/q' <Makefile >tmp_make
	$(CPP) $(CFLAGS) -MM *.c >>tmp_make
	mv tmp_make Makefile
