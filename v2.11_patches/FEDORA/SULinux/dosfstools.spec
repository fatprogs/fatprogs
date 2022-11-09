Name: dosfstools
Summary: Utilities for making and checking MS-DOS FAT filesystems on Linux.
Version: 2.11
Release: 6.2%{?dist}
License: GPL
Group: Applications/System
Source: ftp://ftp.uni-erlangen.de/pub/Linux/LOCAL/dosfstools/dosfstools-%{version}.src.tar.gz
Patch1: dosfstools-2.7-argfix.patch
Patch2: dosfstools-2.11-assumeKernel26.patch
Patch4: dosfstools-2.11-fortify.patch
Patch5: dosfstools-2.11-label.patch
BuildRoot: %{_tmppath}/%{name}-root
Obsoletes: mkdosfs-ygg

%description
The dosfstools package includes the mkdosfs and dosfsck utilities,
which respectively make and check MS-DOS FAT filesystems on hard
drives or on floppies.

%prep
%setup -q
%patch1 -p1 -b .argfix
%patch2 -p1 -b .assumeKernel26
%patch4 -p1 -b .fortify
%patch5 -p1 -b .label

%build
make %{?_smp_mflags} CFLAGS="$RPM_OPT_FLAGS -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64"

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/sbin
mkdir -p %{buildroot}/%{_mandir}/man8

install -m755 -s mkdosfs/mkdosfs %{buildroot}/sbin/mkdosfs
ln %{buildroot}/sbin/mkdosfs %{buildroot}/sbin/mkfs.vfat
ln %{buildroot}/sbin/mkdosfs %{buildroot}/sbin/mkfs.msdos

install -m755 -s dosfsck/dosfsck %{buildroot}/sbin/dosfsck
ln %{buildroot}/sbin/dosfsck %{buildroot}/sbin/fsck.msdos
ln %{buildroot}/sbin/dosfsck %{buildroot}/sbin/fsck.vfat

install -m755 -s dosfsck/dosfslabel %{buildroot}/sbin/dosfslabel

install -m 644 mkdosfs/mkdosfs.8 %{buildroot}%{_mandir}/man8
ln -sf mkdosfs.8.gz %{buildroot}%{_mandir}/man8/mkfs.msdos.8.gz
ln -sf mkdosfs.8.gz %{buildroot}%{_mandir}/man8/mkfs.vfat.8.gz

install -m 644 dosfsck/dosfsck.8 %{buildroot}%{_mandir}/man8
ln -sf dosfsck.8.gz %{buildroot}%{_mandir}/man8/fsck.vfat.8.gz

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root)
/sbin/*
%{_mandir}/man8/*

%changelog
* Thu Jan 11 2007 Peter Jones <pjones@redhat.com> - 2.11-6.2
- Add fs label support.
  Related: #218957

* Wed Jul 12 2006 Jesse Keating <jkeating@redhat.com> - 2.11-6.1
- rebuild

* Fri Jun 30 2006 Peter Vrabec <pvrabec@redhat.com> 2.11-6
- fix upgrade path (#197231)

* Thu May 11 2006 Peter Vrabec <pvrabec@redhat.com> 2.11-5
- fix work with disk image files > 4GB (#191198)

* Fri Feb 10 2006 Jesse Keating <jkeating@redhat.com> - 2.11-4.2
- bump again for double-long bug on ppc(64)

* Tue Feb 07 2006 Jesse Keating <jkeating@redhat.com> - 2.11-4.1
- rebuilt for new gcc4.1 snapshot and glibc changes

* Sun Dec 16 2005 Jakub Jelinek <jakub@redhat.com> 2.11-4
- rebuilt with GCC 4.1
- make it build with -D_FORTIFY_SOURCE=2

* Sun Nov 06 2005 Peter Vrabec <pvrabec@redhat.com> 2.11-3
- fix LFS (#172369)

* Fri Nov 04 2005 Peter Vrabec <pvrabec@redhat.com> 2.11-2
- fix LFS

* Wed Oct 12 2005 Peter Vrabec <pvrabec@redhat.com> 2.11-1
- upgrade

* Thu Apr 28 2005 Peter Vrabec <pvrabec@redhat.com> 2.10-3
- if HDIO_GETGEO fails, print a warning and default to H=255,S=63 (#155950)

* Thu Mar 17 2005 Peter Vrabec <pvrabec@redhat.com> 2.10-2
- rebuild

* Thu Dec 09 2004 Peter Vrabec <pvrabec@redhat.com>  2.10-1
- updated to 2.10

* Wed Nov 10 2004 Martin Stransky <stransky@redhat.com> 2.8-16
- add check for minimum count of clusters in FAT16 and FAT32

* Wed Oct 13 2004 Peter Vrabec <pvrabec@redhat.com> 2.8-15
- fix fat_length type in boot.c. (same problem like in RHEL bug #135293)

* Wed Oct  6 2004 Jeremy Katz <katzj@redhat.com> - 2.8-14
- fix rebuilding (#134834)

* Tue Jun 15 2004 Elliot Lee <sopwith@redhat.com>
- rebuilt

* Fri Feb 13 2004 Elliot Lee <sopwith@redhat.com>
- rebuilt

* Wed Sep  3 2003 Bill Nottingham <notting@redhat.com> 2.8-11
- rebuild

* Wed Sep  3 2003 Bill Nottingham <notting@redhat.com> 2.8-10
- don't rely on <linux/msdos_fs.h> including <asm/byteorder.h>

* Tue Jun 24 2003 Jeremy Katz <katzj@redhat.com> 2.8-9
- rebuild

* Tue Jun 24 2003 Jeremy Katz <katzj@redhat.com> 2.8-8
- add patch from Vince Busam (http://www.sixpak.org/dosfstools/) to do auto 
  creation of FAT32 on large devices (#97308)

* Wed Jun 04 2003 Elliot Lee <sopwith@redhat.com>
- rebuilt

* Wed Feb 19 2003 Jeremy Katz <katzj@redhat.com> 2.8-6
- handle getting the size of loop devices properly (part of #84351)

* Wed Jan 22 2003 Tim Powers <timp@redhat.com>
- rebuilt

* Thu Dec 12 2002 Elliot Lee <sopwith@redhat.com> 2.8-4
- Patch2 for errno

* Fri Jun 21 2002 Tim Powers <timp@redhat.com>
- automated rebuild

* Thu May 23 2002 Tim Powers <timp@redhat.com>
- automated rebuild

* Thu Mar 07 2002 Florian La Roche <Florian.LaRoche@redhat.de>
- update to version 2.8

* Fri Jul  6 2001 Preston Brown <pbrown@redhat.com>
- major upgrade to v2.7.
- forward port old ia64 patch (now incorporated) s390 additions

* Tue Mar 20 2001 Oliver Paukstadt <oliver.paukstadt@millenux.com>
- ported to zSeries (64 bit)

* Wed Jul 12 2000 Prospector <bugzilla@redhat.com>
- automatic rebuild

* Fri Jun 17 2000 Bill Nottingham <notting@redhat.com>
- hard link mkdosfs

* Thu Jun 15 2000 Matt Wilson <msw@redhat.com>
- FHS
- patch to build against 2.4 kernel headers (patch3)

* Fri Apr 28 2000 Bill Nottingham <notting@redhat.com>
- fix for ia64

* Thu Feb  3 2000 Matt Wilson <msw@redhat.com>
- remove mkdosfs.8 symlink, symlink mkdosfs.8.gz to mkfs.msdos.8.gz

* Wed Feb 02 2000 Cristian Gafton <gafton@redhat.com>
- fix descriptions and summary
- man pages are compressed

* Thu Dec 16 1999 Cristian Gafton <gafton@redhat.com>
- fix the 2.88MB drives (patch from hjl)

* Mon Aug 16 1999 Matt Wilson <msw@redhat.com>
- updated to 2.2

* Sun Jun 27 1999 Matt Wilson <msw@redhat.com>
- changed to new maintainer, renamed to dosfstools

* Sat Apr 17 1999 Jeff Johnson <jbj@redhat.com>
- fix mkdosfs on sparc (#1746)

* Sun Mar 21 1999 Cristian Gafton <gafton@redhat.com> 
- auto rebuild in the new build environment (release 10)

* Thu Jan 21 1999 Bill Nottingham <notting@redhat.com>
- build for RH 6.0

* Tue Oct 13 1998 Cristian Gafton <gafton@redhat.com>
- avoid using unsinged long on alphas 

* Sun Aug 16 1998 Jeff Johnson <jbj@redhat.com>
- build root

* Mon Apr 27 1998 Prospector System <bugs@redhat.com>
- translations modified for de, fr, tr

* Thu Jul 10 1997 Erik Troan <ewt@redhat.com>
- built against glibc
