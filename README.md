# Overview

fatprogs is FAT32 utility project by being developed from dosfstools v2.11,
which is released under the last version of GPL v2 license.

fatprogs support mkdosfs, dosfsck, dosfslabel like dosfstools and additionally
dosfsdump for debugging.

dosfstools is excellent project and developed and verified for long time.
But, found some issues when it is applied to embedded device. In test with 4K
cluster on 32G volume, checked that dosfstools uses very large memory.(80M above)
So, it may be hard to use for embedded device with small resources.

Also, there is even issue of license. Many embedded companies are hesitant to
use software with a GPL v3 due to it's restriction. So some utilities including
dosfstools are also provided as a yocto meta layer to preserve GPL v2 license.
(meta-gplv2). But their codes are very old at leat 7~10 years ago.

To resolve above issues, fatprogs is developed from dosfstools v2.11 and
patches of distribution vendors based on v2.11. And found missing features
through comparison of corrupted images' recovery results with serveral FAT32
tools existing in linux and Windows chkdsk utility. Also I checked published
documents for fat32 specification.

After adding missing features, optimized to use less memory. Verified the MAX RSS
memory was reduced by about 90% compared to latest dosfstools version(v4.2).
Tested results are attached below.

# Build & Installation

This project now uses the GNU autotools build system (autoconf / automake / libtool).

You should generate 'configure' and 'Makefile' files first by running:

```
./autogen.sh
```

Then configure, build and install as usual:

```
./configure [--prefix=/usr/local]    # see ./configure --help for options
make
make install
```

Cleaning targets provided by the generated Makefiles:

```
make clean      # remove object files
make distclean  # remove generated files (configure, Makefile, etc.)
```

Note: If libblkid is not available, features that filesystem detection in mkdosfs will be disabled.

## Debug & AddressSanitizer builds

Pass CFLAGS/LDFLAGS to configure to enable debug or sanitizers. Examples:

Debug build (no optimizations, with debug symbols):

```
CFLAGS='-g -O0' ./configure
make
```

address sanitizer build:

```
# Configure and build with address sanitizer enabled

CFLAGS='-g -O1 -fsanitize=address -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address' ./configure --prefix=/usr/local
make -j$(nproc)
```

After building with address sanitizer, you may verify the binaries and test it.

```
ldd src/dosfsck | grep -i asan
file src/dosfsck

# run a quick test (ASAN_OPTIONS can be used to tune output)
ASAN_OPTIONS=verbosity=1 ./src/dosfsck -V /path/to/test.img
```

## Cross-compilation

### Using 'configure' options
Use autotools options for cross-compiling and override CC/CFLAGS/LDFLAGS as needed.
Example:

```
./configure --host=arm-linux-gnueabihf CC=arm-linux-gnueabihf-gcc \
            CFLAGS='-D_FILE_OFFSET_BITS=64 -I/path/to/sysroot/include' \
            LDFLAGS='-L/path/to/sysroot/lib'
make
```

### Using yocto build

When building in yocto, you may need to add following example in your bb/bbappend file to build fatprogs.

```
inherit autotools pkgconfig

SRC_URI = "git://github.com/fatprogs-org/fatprogs.git;branch=main;protocol=https"

S = "${WORKDIR}/git"

CFLAGS += "-D_GNU_SOURCE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -I${S}/include"

EXTRA_OEMAKE = "CC='${CC}' CFLAGS='${CFLAGS}' LDFLAGS='${LDFLAGS}'"

do_install:prepend() {
    if [ -d ${S}/src ] && [ -d ${B}/src ]; then
        for f in ${S}/src/*.8; do
            if [ -f "$f" ]; then
                install -m 644 "$f" ${B}/src/
            fi
        done
    fi
}

```

# Test Result
fatprogs was tested on x86-64, aarch64. fatprogs built and tested
on full 64bit on x86-64, and with -m32 option on aarch64.

## Corrupted Images
Corrupted Images for fatprogs test are in [here](https://github.com/fatprogs/fat32_bad_images)
You may test all corrupted images executing script *test\_all\_images.sh* in tests directory.
Script will clone images from above repository, and test all images.

```
cd tests
./test_all_images
```

Tested images deatils are described in README file above link.

## Comparison of memory usage
One of developement goal of fatprogs was use less resources. The following
result is memory usage of fatprogs and latest dosfstools, mesasured by
'time -v' command for corrupted images.

### Memory Usage (Max Resident Set Size)
Data is average value of each corrupted images category.

<pre>
fat32_bad_ccXX.dump     : 1G volume, 4K cluster, about 20 files
fat32_bad_deXX.dump     : 1G volume, 4K cluster, about 100 files
fat32_bad_entXX.dump    : 1G volume, 4K cluster, about 370 files
fat32_bad_dirtyXX.dump  : 1G volume, 4K cluster, about 100 files
fat32_bad_fsinfoXX.dump : 1G volume, 4K cluster
fat32_bad_lfnXX.dump    : 1G volume, 4K cluster, about 60 files
fat32_bad_volXX.dump    : 1G volume, 4K cluster

fat32_large.dump        : 32G volume, 4K cluster, about 400 files, full
fat32_large_16k.dump    : 32G volume, 16K cluster, about 400 files, full
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

# Report & Contribution
Reporting issues and suggesting features for fatprogs are always welcome.
License of fatprogs is GPL-2.0. So, to avoid license violation and
compatibility, fatprogs can't apply code with GPL-3.0 and other license
which conflict with GPL-2.0.

Somebody who contribute to fatprogs should be careful for that. You can use
*fosslight scanner* to check license for free. (https://fosslight.org/)

# TODO
* More optimization for logic of traversing file tree and memory.
* Improve build script for cross compile.
* fatprogs does not support code page.
