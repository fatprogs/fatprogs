#
# spec file for package dosfstools
#
# Copyright (c) 2011 SUSE LINUX Products GmbH, Nuernberg, Germany.
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

# norootforbuild


Name:           dosfstools
Provides:       mkdosfs dosfsck
License:        GPLv2+
Group:          System/Filesystems
AutoReqProv:    on
Summary:        Utilities for Making and Checking MS-DOS FAT File Systems on Linux
Version:        2.11
Release:        121.122.2
Url:            ftp://ftp.uni-erlangen.de/pub/Linux/LOCAL/dosfstools
Source:         %{name}-%{version}.src.tar.bz2
Patch0:         %{name}-%{version}-linuxfs.patch
Patch1:         %{name}-%{version}-unaligned.patch
Patch2:         %{name}-%{version}-buffer.patch
Patch3:         %{name}-%{version}-o_excl.patch
Patch4:         %{name}-%{version}-mkdosfs-geo0.diff
Patch5:         %{name}-%{version}_determine-sector-size.patch
Patch6:         %{name}-%{version}-unsupported-sector-size.patch
Patch7:         %{name}-%{version}-filename-buffer-overflow.patch
Obsoletes:      mkdosfs dosfsck dosfstls
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
Supplements:    filesystem(vfat)

%description
The dosfstools package includes the mkdosfs and dosfsck utilities,
which respectively make and check MS-DOS FAT file systems on hard
drives or on floppies.



Authors:
--------
    Dave Hudson <dave@humbug.demon.co.uk>
    Werner Almesberger <werner.almesberger@lrc.di.epfl.ch>
    Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>

%prep
%setup
%patch0
%patch1 -p1
%patch2
%patch3
%patch4 -p1
%patch5
%patch6
%patch7

%build
make OPTFLAGS="-D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE $RPM_OPT_FLAGS"

%install
# directories
install -d $RPM_BUILD_ROOT{/sbin,%{_mandir}/man8}
# binaries
install -m755 mkdosfs/mkdosfs $RPM_BUILD_ROOT/sbin/
install -m755 dosfsck/dosfsck $RPM_BUILD_ROOT/sbin/
# alternative names
ln -sf mkdosfs $RPM_BUILD_ROOT/sbin/mkfs.msdos
ln -sf dosfsck $RPM_BUILD_ROOT/sbin/fsck.msdos
ln -sf mkdosfs $RPM_BUILD_ROOT/sbin/mkfs.vfat
ln -sf dosfsck $RPM_BUILD_ROOT/sbin/fsck.vfat
# man pages
install -m 644 mkdosfs/mkdosfs.8 $RPM_BUILD_ROOT%{_mandir}/man8/
install -m 644 dosfsck/dosfsck.8 $RPM_BUILD_ROOT%{_mandir}/man8/
# man pages for alternative names
ln -sf mkdosfs.8.gz $RPM_BUILD_ROOT%{_mandir}/man8/mkfs.msdos.8.gz
ln -sf dosfsck.8.gz $RPM_BUILD_ROOT%{_mandir}/man8/fsck.msdos.8.gz
ln -sf mkdosfs.8.gz $RPM_BUILD_ROOT%{_mandir}/man8/mkfs.vfat.8.gz
ln -sf dosfsck.8.gz $RPM_BUILD_ROOT%{_mandir}/man8/fsck.vfat.8.gz
# documentation
install -m755 -d $RPM_BUILD_ROOT/%{_docdir}/%{name}/dosfsck
install -m755 -d $RPM_BUILD_ROOT/%{_docdir}/%{name}/mkdosfs
install -m644 CHANGES TODO README.Atari $RPM_BUILD_ROOT/%{_docdir}/%{name}/
install -m644 dosfsck/{COPYING,README} $RPM_BUILD_ROOT/%{_docdir}/%{name}/dosfsck
install -m644 mkdosfs/{COPYING,README} $RPM_BUILD_ROOT/%{_docdir}/%{name}/mkdosfs

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc %{_docdir}/%{name}
/sbin/*
%{_mandir}/man8/*.gz

%changelog
* Mon Mar 14 2011 pgajdos@suse.cz
- fixed fsck.vfat crashing [bnc#677236]
* Tue Jun 24 2008 pgajdos@suse.cz
- added warning for creation msdos on filesystem with sector size
  greater than 4096 [fate#303325] (unsupported-sector-size.patch)
* Thu Sep  6 2007 pgajdos@suse.cz
- determine sector size of device automatically or if -S parameter
  present, verify, that it's not under physical sector size
  (determine-sector-size.patch)
* Thu Aug  9 2007 olh@suse.de
- remove inclusion of asm/unaligned.h, use private copy of
  asm-generic/unaligned.h
* Thu Apr 26 2007 lnussel@suse.de
- remove obsolete subfs patch
- fix handling of zero heads and sectors
* Wed Apr  4 2007 pgajdos@suse.cz
- added Supplements: filesystem(vfat) line to spec file
  [fate#301966]
* Tue Jan 30 2007 prusnak@suse.cz
- mkdosfs now opens device with O_EXCL [#238687]
* Sat May 27 2006 schwab@suse.de
- Don't strip binaries.
* Wed Jan 25 2006 mls@suse.de
- converted neededforbuild to BuildRequires
* Tue Nov  8 2005 dmueller@suse.de
- don't build as root
* Mon Nov  7 2005 yxu@suse.de
- fixed overflowing buffer problem
* Mon Apr 11 2005 mcihar@suse.cz
- update to 2.11
- use sys/mount.h instead of linux/fs.h, this fixes compilation with current GCC
* Tue Feb 15 2005 mcihar@suse.cz
- deal with subfs (bug #50838)
  - use /proc/mounts if available for deciding whether device is
    mounted or not
  - just issue warning if it is mounted as subfs
* Thu Aug 19 2004 mcihar@suse.cz
- merged some dosfsck fixes from FreeDOS
* Thu Jul 15 2004 schwab@suse.de
- Fix unaligned accesses [#40296].
* Tue Jun  1 2004 ro@suse.de
- avoid inclusion of linux/audit.h
* Thu Mar 18 2004 mcihar@suse.cz
- fix dosfsck man page (pointed out in bug #34757)
* Mon Mar  8 2004 mcihar@suse.cz
- fix broken dosfsck (bug #34757)
* Thu Jan 29 2004 mcihar@suse.cz
- include more documentation
* Thu Jan 15 2004 kukuk@suse.de
- Make compile with kernel 2.6.1 headers
* Thu Oct 23 2003 schwab@suse.de
- Don't define llseek to lseek64, creates infinite recursion.
* Tue Oct 14 2003 mcihar@suse.cz
- install links also for {fsck,mkfs}.vfat + man pages (bug #32284)
* Mon Sep 29 2003 mcihar@suse.cz
- updated to 2.10:
  - dosfsck: various 64-bit fixes and removed some warnings by Michal
  Cihar <mcihar@suse.cz>
  - mkdosfs: better error message if called without parameters (also
  suggested by Michal)
* Mon Jun  9 2003 mcihar@suse.cz
- new upstream version 2.9:
  * dosfsck: Fix potential for "Internal error: next_cluster on bad cluster".
  * dosfsck: When clearing long file names, don't overwrite the dir
  entries with all zeros, but put 0xe5 into the first byte.
  Otherwise, some OSes stop reading the directory at that point...
  * dosfsck: in statistics printed by -v, fix 32bit overflow in number
  of data bytes.
  * dosfsck: fix an potential overflow in "too many clusters" check
  * dosfsck: allow FAT size > 32MB.
  * dosfsck: allow for only one FAT
  * dosfsck: with -v, also check that last sector of the filesystem can
  be read (in case a partition is smaller than the fs thinks)
- realy working large file support
- don't package obsolette documentation
* Wed Dec  4 2002 mcihar@suse.cz
- don't allow -fPIC on i386 in CFLAGS, even on i386-debug, because
  this package doesn't build with it
* Mon Dec  2 2002 ro@suse.de
- include errno.h where needed
* Tue Sep 10 2002 mcihar@suse.cz
- added -D_FILE_OFFSET_BITS=64 to CFLAGS to support larger files/partitions
* Tue May 21 2002 ro@suse.de
- extend 64bit ifdefs for new platforms
* Fri Mar  1 2002 jantos@suse.cz
- Fixed missing files in documentation (bug 13973)
* Fri Sep 14 2001 schwab@suse.de
- Fix crash if mkdosfs is called without arguments.
* Tue May 22 2001 pblaha@suse.cz
- fixed include files on ia64
* Sun Apr  8 2001 schwab@suse.de
- Fix to build on ia64.
* Mon Mar  5 2001 pblaha@suse.cz
- update on 2.8
* Mon Feb 12 2001 ro@suse.de
- don't include linux/fs.h
* Thu Jan 18 2001 schwab@suse.de
- Add Obsoletes: dosfstls.
* Wed Jan 17 2001 pblaha@suse.cz
- added message  "not enough memory to run dosfsck\n"
- if not free memory for malloc
* Fri Dec 22 2000 pblaha@suse.cz
- upgrade on 2.6 and rename on dosfstools
* Mon Dec  4 2000 sf@suse.de
- corrected patch to compile on Alpha and ia64
* Tue Nov 21 2000 uli@suse.de
- worked around strncasecmp declaration conflict in mkdosfs.c
* Mon Nov 13 2000 ro@suse.de
- hacked to compile on 2.4 includes
* Thu Nov  2 2000 pblaha@suse.cz
- update to version 2.4
* Mon Jun  5 2000 schwab@suse.de
- Fix llseek on ia64.
* Fri Jun  2 2000 bubnikv@suse.cz
- new package in SuSE, version 2.2
- makes packages dosfsck and mkdosfs obsolette
