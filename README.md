# Overview

fatprogs is FAT32 utility project by being developed from dosfstools v2.11,
which is released under the last version of GPL v2 license.

fatprogs support mkdosfs, dosfsck, dosfslabel like dosfstoos and additionally
dosfsdump for debugging.

dosfstools is excellent project and developed and verified for long time.
But, found some issues when it is applied to embedded device. In test with 4K
cluster on 32G volume, checked that dosfstools uses at least 120M memory above.
So, it may be hard to use for embedded device with small resource.

Also, there is even issue of license. Many embedded companies are hesitant to
use software with a GPL v3 due to it's restriction. So some utilities including
dosfstools are also provided as a yocto meta layer to preserve GPL v2 license.
(meta-gplv2). But their codes are very old at leat 7~10 years ago.

To resolve above issues, fatprogs is developed from dosfstools v2.11 and
patches of distribution vendors based on v2.11. And found missing features
through comparison of corrupted images' recovery results with serveral FAT32
tools existing in linux and Windows chkdsk utility.

After adding missing features, optimized to use less memory. Verified the MAX RSS
memory was reduced by about 90% compared to latest dosfstools version(v4.2).
Tested results are attached below.


# Report & Contribution
Reporting issues and suggesting features for fatprogs are always welcome.
License of fatprogs is GPL-2.0. So, to avoid license violation and
compatibility, fatprogs can't apply code with GPL-3.0 and other license
which conflict with GPL-2.0.

Somebody who contribute to fatprogs should be careful for that. You can use
fosslight to check license for free. (https://fosslight.org)

# Build & Installation

Build target 'distclean' will remove all object and execution files. And
'clean' will remove all object files except execution files.

```
$ make [distclean | clean]

$ make
$ make install
```

## Other build target
'asan' represent building with address sanitizer library for runtime memory debugging.
'debug' build will include debug information to trace code well with tools like gdb.

```
$ make debug
$ make asan
```

# Yocto build
fatprogs verified the cross build test only in yocto. You may should add below
statement in your bb/bbappend file to build fatprogs, which refer to dosfstools bb file
in meta-gplv2.

```
SRC_URI = "git://github.com/jaysim/fatprogs.git;branch=main;protocol=https"

S = "${WORKDIR}/git"

CFLAGS += "-D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -I${S}/include"

TARGET_CC_ARCH += "${LDFLAGS}"
EXTRA_OEMAKE = "CC='${CC}' CFLAGS='${CFLAGS}' LDFAGS='${LDFLAGS}'"

do_install() {
        oe_runmake "PREFIX=${D}" "SBINDIR=${D}${base_sbindir}" \
                "MANDIR=${D}${mandir}/man8" install
}

```

# Test Result
fatprogs was tested on x86-64, aarch64. fatprogs built and tested
on full 64bit on x86-64, and with -m32 option on aarch64.

## Corrupted Images
test script for fatprogs are in
git@github.com:jaysim/fat32\_bad_images.git repository.

Corrupted images can't share through github, because of github size limitation.
So, just share raw data of corrupte images and test script.
Tested images deatils are described in README file above repository.

## Comparison of memory usage
One of developement goal of fatprogs was use less resources. The following
result is memory usage of fatprogs and latest dosfstools, mesasured by
'time -v' command for corrupted images.

### Memory Usage (Max Resident Set Size)
Data is average value of each corrupted images category.

<pre>
fat32\_bad\_ccXX.dump     : 1G volume, 4K cluster, about 20 files
fat32\_bad\_deXX.dump     : 1G volume, 4K cluster, about 100 files
fat32\_bad\_entXX.dump    : 1G volume, 4K cluster, about 370 files
fat32\_bad\_dirtyXX.dump  : 1G volume, 4K cluster, about 100 files
fat32\_bad\_fsinfoXX.dump : 1G volume, 4K cluster
fat32\_bad\_lfnXX.dump    : 1G volume, 4K cluster, about 60 files
fat32\_bad\_volXX.dump    : 1G volume, 4K cluster

fat32\_large.dump        : 32G volume, 4K cluster, about 400 files, full
fat32\_large\_16k.dump    : 32G volume, 16K cluster, about 400 files, full
</pre>

	Memory Usage (unit: KB - ratio: fatprogs/dosfstools value)
	+----------------------------------------------------------+
	| Images                  | fatprogs  | dosfstools | ratio |
	|-------------------------+-----------+------------+-------|
	| fat32_bad_ccXX.dump	  | 2224      | 6361       | 35%   |
	| fat32_bad_deXX.dump     | 2208      | 6363       | 35%   |
	| fat32_bad_entXX.dump    | 3963      | 34519      | 12%   |
	| fat32_bad_dirtyXX.dump  | 2201      | 6369       | 35%   |
	| fat32_bad_fsinfoXX.dump | 1930      | 4358       | 45%   |
	| fat32_bad_lfnXX.dump    | 2190      | 6308       | 35%   |
	| fat32_bad_volXX.dump    | 2179      | 6287       | 35%   |
	|-------------------------+-----------+------------+-------|
	| fat32_large.dump        | 9944      | 100404     | 10%   |
	| fat32_large_16K.dump    | 3708      | 26820      | 14%   |
	+----------------------------------------------------------+

dosfstools need about 100M memory in worst case, so embedded device might not
be able to handle this type of FAT32 device. Of course, fatprogs memory also
need to optimize more.

TODO
====
* more optimization for logic of traversing file tree and memory.
* Improve build script for cross compile.
* fatprogs does not support code page.
