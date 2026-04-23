# Programs
bin_PROGRAMS = dosfsck dosfslabel dosfsdump mkdosfs

dosfsck_SOURCES = common.c boot.c check.c fat.c file.c io.c lfn.c dosfsck.c
dosfslabel_SOURCES = common.c boot.c check.c fat.c file.c io.c lfn.c dosfslabel.c
dosfsdump_SOURCES = common.c dosfsdump.c
mkdosfs_SOURCES = common.c mkdosfs.c
mkdosfs_LDADD = $(BLKID_LIBS)

# Add include directory to CFLAGS
AM_CFLAGS = -I$(top_srcdir)/include $(BLKID_CFLAGS)

# Custom installation logic for symlinks and man pages
install-exec-am:
	mkdir -p $(DESTDIR)$(mandir)
	mkdir -p $(DESTDIR)$(sbindir)
	install -m 644 dosfsck.8 $(DESTDIR)$(mandir)/dosfsck.8
	install -m 644 mkdosfs.8 $(DESTDIR)$(mandir)/mkdosfs.8
	ln -sf dosfsck $(DESTDIR)$(sbindir)/fsck.msdos
	ln -sf dosfsck $(DESTDIR)$(sbindir)/fsck.vfat
	ln -sf dosfsck.8 $(DESTDIR)$(mandir)/fsck.msdos.8
	ln -sf dosfsck.8 $(DESTDIR)$(mandir)/fsck.vfat.8
	ln -sf mkdosfs $(DESTDIR)$(sbindir)/mkfs.msdos
	ln -sf mkdosfs $(DESTDIR)$(sbindir)/mkfs.vfat
	ln -sf mkdosfs.8 $(DESTDIR)$(mandir)/mkfs.msdos.8
	ln -sf mkdosfs.8 $(DESTDIR)$(mandir)/mkfs.vfat.8
