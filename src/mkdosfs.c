/* SPDX-License-Identifier : GPL-2.0 */

/*
Filename:     mkdosfs.c
Version:      0.3b (Yggdrasil)
Author:       Dave Hudson
Started:      24th August 1994
Last Updated: 7th May 1998
Updated by:   Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de>
Target O/S:   Linux (2.x)

Description: Utility to allow an MS-DOS filesystem to be created
under Linux.  A lot of the basic structure of this program has been
borrowed from Remy Card's "mke2fs" code.

As far as possible the aim here is to make the "mkdosfs" command
look almost identical to the other Linux filesystem make utilties,
eg bad blocks are still specified as blocks, not sectors, but when
it comes down to it, DOS is tied to the idea of a sector (512 bytes
as a rule), and not the block.  For example the boot block does not
occupy a full cluster.

Fixes/additions May 1998 by Roman Hodek
<Roman.Hodek@informatik.uni-erlangen.de>:
- Atari format support
- New options -A, -S, -C
- Support for filesystems > 2GB
- FAT32 support

Fixes/additions June 2003 by Sam Bingner
<sam@bingner.com>:
- Add -B option to read in bootcode from a file
- Write BIOS drive number so that FS can properly boot
- Set number of hidden sectors before boot code to be one track

Copying:     Copyright 1993, 1994 David Hudson (dave@humbug.demon.co.uk)

Portions copyright 1992, 1993 Remy Card (card@masi.ibp.fr)
and 1991 Linus Torvalds (torvalds@klaava.helsinki.fi)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/* Include the header files */

#include "version.h"

#include <fcntl.h>
#include <linux/hdreg.h>
#include <sys/mount.h>
#include <linux/fd.h>
#include <endian.h>
#include <mntent.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include <asm/types.h>

#include "dosfs.h"
#include "common.h"

/* In earlier versions, an own llseek() was used, but glibc lseek() is
 * sufficient (or even better :) for 64 bit offsets in the meantime */
#define llseek lseek

#define TEST_BUFFER_BLOCKS 16
#define HARD_SECTOR_SIZE   512
#define SECTORS_PER_BLOCK (BLOCK_SIZE / HARD_SECTOR_SIZE)

/* Macro definitions */
/* Mark a cluster in the FAT as bad */
#define mark_sector_bad(sector) mark_FAT_sector(sector, VALUE_FAT_BAD)

/* Compute ceil(a/b) */
inline __attribute__((always_inline)) int cdiv(int a, int b)
{
    return (a + b - 1) / b;
}

/* FAT values */
#define VALUE_FAT_EOF   (atari_format ? 0x0fffffff : 0x0ffffff8)
#define VALUE_FAT_BAD   0x0ffffff7

#define HD_DRIVE_NUMBER 0x80	/* Boot off first hard drive */
#define FD_DRIVE_NUMBER 0x00	/* Boot off first floppy drive */

/* comments from fat specification documents(fatgen103.doc)
 *
 * This is the one and only way that FAT type is determined.
 * There is no such thing as a FAT12 volume that has more than 4084 clusters.
 * There is no such thing as a FAT16 volume that has less than 4085 clusters or
 * more than 65,524 clusters. There is no such thing as a FAT32 volume
 * that has less than 65,525 clusters. If you try to make a FAT volume
 * that violates this rule, Microsoft operating systems will not handle
 * them correctly because they will think the volume has a different type
 * of FAT than what you think it does.
 *
 * NOTE: As is noted numerous times earlier, the world is full of FAT code
 * that is wrong. There is a lot of FAT type code that is off by 1 or 2 or
 * 8 or 10 or 16. For this reason, it is highly recommended that if you are
 * formatting a FAT volume which has maximum compatibility with all existing
 * FAT code, then you should you avoid making volumes of any type that have
 * close to 4,085 or 65,525 clusters. Stay at least 16 clusters on each side
 * away from these cut-over cluster counts.
 */
#define MAX_CLUST_12	((1 << 12) - 16)
#define MAX_CLUST_16	((1 << 16) - 16)
#define MIN_CLUST_32    65529
/* M$ says the high 4 bits of a FAT32 FAT entry are reserved and don't belong
 * to the cluster number. So the max. cluster# is based on 2^28 */
#define MAX_CLUST_32	((1 << 28) - 16)

#define FAT12_THRESHOLD	4085

#define OLDGEMDOS_MAX_SECTORS	32765
#define GEMDOS_MAX_SECTORS	65531
#define GEMDOS_MAX_SECTOR_SIZE	(16 * 1024)

#define MAX_RESERVED		0xFFFF

/* The "boot code" we put into the filesystem... it writes a message and
   tells the user to try again */
char dummy_boot_jump[3] = { 0xeb, 0x3c, 0x90 };

char dummy_boot_jump_m68k[2] = { 0x60, 0x1c };

#define MSG_OFFSET_OFFSET 3
char dummy_boot_code[BOOTCODE_SIZE] =
"\x0e"          /* push cs */
"\x1f"          /* pop ds */
"\xbe\x5b\x7c"  /* mov si, offset message_txt */

/* write_msg: */
"\xac"          /* lodsb */
"\x22\xc0"      /* and al, al */
"\x74\x0b"      /* jz key_press */
"\x56"          /* push si */
"\xb4\x0e"      /* mov ah, 0eh */
"\xbb\x07\x00"  /* mov bx, 0007h */
"\xcd\x10"      /* int 10h */
"\x5e"          /* pop si */
"\xeb\xf0"      /* jmp write_msg */

/* key_press: */
"\x32\xe4"      /* xor ah, ah */
"\xcd\x16"      /* int 16h */
"\xcd\x19"      /* int 19h */
"\xeb\xfe"      /* foo: jmp foo */
/* message_txt: */

"This is not a bootable disk.  Please insert a bootable floppy and\r\n"
"press any key to try again ... \r\n";

#define MESSAGE_OFFSET 29	/* Offset of message in above code */

#define error(str)				\
    do {						\
        free_mem(fat);					\
        if (fsinfo)        \
            free_mem(fsinfo);	\
        free_mem(root_dir);				\
        die(str);					\
    } while (0)

#define seekto(pos,errstr)						\
    do {									\
        loff_t __pos = (pos);						\
        if (llseek(dev, __pos, SEEK_SET) != __pos)				\
            error("seek to " errstr " failed whilst writing tables");	\
    } while (0)

#define writebuf(buf, size, errstr)			\
    do {							\
        int __size = (size);				\
        if (write(dev, buf, __size) != __size)		\
            error("failed whilst writing " errstr);	\
    } while (0)

#define pwritebuf(buf, size, pos, errstr)			\
    do {							\
        int __size = (size);				\
        if (pwrite(dev, buf, __size, pos) != __size)		\
            error("failed whilst writing " errstr);	\
    } while (0)
/* Global variables - the root of all evil :-) - see these and weep! */

static char *template_boot_code;	/* Variable to store a full template boot sector in */
static int use_template = 0;
static char *program_name = "mkdosfs";	/* Name of the program */
static char *device_name = NULL;	/* Name of the device on which to create the filesystem */
static int check = FALSE;	/* Default to no readablity checking */
static int verbose = 0;		/* Default to verbose mode off */
static long volume_id;		/* Volume ID number */
static time_t create_time;	/* Creation time */
static char volume_name[] = LABEL_NONAME; /* Volume name */
static unsigned long long blocks;	/* Number of blocks in filesystem */
static int sector_size = 512;	/* Size of a logical sector */
static int sector_size_set = 0; /* User selected sector size */
static int backup_boot = 0;	/* Sector# of backup boot sector */
static int reserved_sectors = 0;/* Number of reserved sectors */
static int badblocks = 0;	/* Number of bad blocks in the filesystem */
static int nr_fats = 2;		/* Default number of FATs to produce */
static int fat_bits = 0;	/* Size in bits of FAT entries */
static int size_fat_by_user = 0; /* 1 if FAT size user selected */
static int dev = -1;		/* FS block device file handle */
static int  ignore_full_disk = 0; /* Ignore warning about 'full' disk devices */
static off_t currently_testing = 0;	/* Block currently being tested (if autodetect bad blocks) */
static struct boot_sector bs;	/* Boot sector data */
static int start_data_sector;	/* Sector number for the start of the data area */
static int start_data_block;	/* Block number for the start of the data area */
static unsigned char *fat = NULL;	/* File allocation table */
static unsigned char *fsinfo;	/* FAT32 info sector */
static struct dir_entry *root_dir;	/* Root directory */
static int size_root_dir;	/* Size of the root directory in bytes */
static int sectors_per_cluster = 0;	/* Number of sectors per disk cluster */
static int root_dir_entries = 0;	/* Number of root directory entries */
static char *blank_sector;		/* Blank sector - all zeros */
static int hidden_sectors = 0;		/* Number of hidden sectors */

static unsigned int fat_size;   /* size of FAT (bytes) */
static off_t fat_start;     /* start offset of FAT */

/* Function prototype definitions */

static void fatal_error(const char *fmt_string) __attribute__((noreturn));
static void mark_FAT_cluster(int cluster, unsigned int value);
static void mark_FAT_sector(int sector, unsigned int value);
static long do_check(char *buffer, int try, off_t current_block);
static void alarm_intr(int alnum);
static void check_blocks(void);
static void get_list_blocks(char *filename);
static int valid_offset(int fd, loff_t offset);
static unsigned long long count_blocks(char *filename);
static void check_mount(char *device_name);
static void establish_params(int device_num, int size);
static void setup_tables(void);
static void write_tables(void);

int atari_format = 0;	/* Use Atari variation of MS-DOS FS format */

/* The function implementations */

/* Handle the reporting of fatal errors.
 * Volatile to let gcc know that this doesn't return */
static void fatal_error(const char *fmt_string)
{
    fprintf(stderr, fmt_string, program_name, device_name);
    exit(1);        /* The error exit code is 1! */
}

/* Mark the specified cluster as having a particular value */
static void mark_FAT_cluster(int cluster, unsigned int value)
{
    unsigned char data[4];
    uint32_t saved;
    int i;

    saved = value;
    switch (fat_bits) {
        case 12:
            value &= 0x0fff;
            if (((cluster * 3) & 0x1) == 0) {
                fat[3 * cluster / 2] = (unsigned char)(value & 0x00ff);
                fat[(3 * cluster / 2) + 1] =
                    (unsigned char)((fat[(3 * cluster / 2) + 1] & 0x00f0) |
                            ((value & 0x0f00) >> 8));
            }
            else {
                fat[3 * cluster / 2] =
                    (unsigned char)((fat[3 * cluster / 2] & 0x000f) |
                            ((value & 0x000f) << 4));
                fat[(3 * cluster / 2) + 1] =
                    (unsigned char)((value & 0x0ff0) >> 4);
            }
            break;

        case 16:
            value &= 0xffff;
            fat[2 * cluster] = (unsigned char)(value & 0x00ff);
            fat[(2 * cluster) + 1] = (unsigned char)(value >> 8);
            break;

        case 32:
            value &= 0xfffffff;
            *(uint32_t *)data =
                CT_LE_L((value & 0xfffffff) | (saved & 0xf0000000));

            for (i = 0; i < nr_fats; i++) {
                pwritebuf(&data, 4, (fat_size * i) + fat_start + 4 * cluster,
                        "mark_FAT_cluster");
            }
            break;

        default:
            die("Bad FAT size (not 12, 16, or 32)");
    }
}

/* Mark a specified sector as having a particular value in it's FAT entry */
static void mark_FAT_sector(int sector, unsigned int value)
{
    int cluster;

    cluster = (sector - start_data_sector) / (int)(bs.sec_per_clus) /
        (sector_size / HARD_SECTOR_SIZE);

    if (cluster < 0)
        die("Invalid cluster number in mark_FAT_sector: probably bug!");

    mark_FAT_cluster(cluster, value);
}

/* Perform a test on a block.
 * Return the number of blocks that could be read successfully */
static long do_check(char *buffer, int try, off_t current_block)
{
    long got;

    /* Try reading! */
    got = pread(dev, buffer, try * BLOCK_SIZE, current_block * BLOCK_SIZE);
    if (got < 0)
        got = 0;

    if (got & (BLOCK_SIZE - 1))
        printf("Unexpected values in do_check: probably bugs\n");

    got /= BLOCK_SIZE;

    return got;
}

/* Alarm clock handler - display the status of the quest for bad blocks!
 * Then retrigger the alarm for five senconds later
 * (so we can come here again) */
static void alarm_intr(int alnum)
{
    if (currently_testing >= blocks)
        return;

    signal(SIGALRM, alarm_intr);
    alarm(5);

    if (!currently_testing)
        return;

    printf("%lld... ", (unsigned long long)currently_testing);
    fflush(stdout);
}

static void check_blocks(void)
{
    int try, got;
    int i;
    static char blkbuf[BLOCK_SIZE * TEST_BUFFER_BLOCKS];

    if (verbose) {
        printf("Searching for bad blocks ");
        fflush(stdout);
    }

    currently_testing = 0;
    if (verbose) {
        signal (SIGALRM, alarm_intr);
        alarm (5);
    }

    try = TEST_BUFFER_BLOCKS;
    while (currently_testing < blocks) {
        if (currently_testing + try > blocks)
            try = blocks - currently_testing;

        got = do_check (blkbuf, try, currently_testing);
        currently_testing += got;
        if (got == try) {
            try = TEST_BUFFER_BLOCKS;
            continue;
        }
        else
            try = 1;

        if (currently_testing < start_data_block)
            die("bad blocks before data-area: cannot make fs");

        /* Mark all of the sectors in the block as bad */
        for (i = 0; i < SECTORS_PER_BLOCK; i++)
            mark_sector_bad(currently_testing * SECTORS_PER_BLOCK + i);

        badblocks++;
        currently_testing++;
    }

    if (verbose)
        printf("\n");

    if (badblocks)
        printf("%d bad block%s\n", badblocks, (badblocks > 1) ? "s" : "");
}

static void get_list_blocks(char *filename)
{
    int i;
    FILE *listfile;
    unsigned long blockno;

    listfile = fopen(filename, "r");
    if (listfile == (FILE *)NULL)
        die("Can't open file of bad blocks");

    while (!feof(listfile)) {
        int ret;

        errno = 0;
        ret = fscanf(listfile, "%ld\n", &blockno);
        if (errno != 0) {
            printf("fscanf error(%d:%s)\n", ret, __func__);
        }

        /* Mark all of the sectors in the block as bad */
        for (i = 0; i < SECTORS_PER_BLOCK; i++)
            mark_sector_bad(blockno * SECTORS_PER_BLOCK + i);

        badblocks++;
    }
    fclose(listfile);

    if (badblocks)
        printf("%d bad block%s\n", badblocks, (badblocks > 1) ? "s" : "");
}

/* Given a file descriptor and an offset,
 * check whether the offset is a valid offset for the file -
 * return FALSE if it isn't valid or TRUE if it is */
static int valid_offset(int fd, loff_t offset)
{
    char ch;

    if (pread(fd, &ch, 1, offset) < 1)
        return FALSE;

    return TRUE;
}

/* Given a filename, look to see how many blocks of BLOCK_SIZE are present,
 * returning the answer */
static unsigned long long count_blocks(char *filename)
{
    off_t high, low;
    int fd;

    if ((fd = open(filename, O_RDONLY)) < 0) {
        perror(filename);
        exit(1);
    }

    /* first try SEEK_END, which should work on most devices nowadays */
    if ((low = llseek(fd, 0, SEEK_END)) <= 0) {
        low = 0;
        for (high = 1; valid_offset(fd, high); high *= 2)
            low = high;

        while (low < high - 1) {
            const loff_t mid = (low + high) / 2;
            if (valid_offset(fd, mid))
                low = mid;
            else
                high = mid;
        }
        ++low;
    }

    close(fd);
    return low / BLOCK_SIZE;
}

/* Check to see if the specified device is currently mounted - abort if it is */
static void check_mount(char *device_name)
{
    FILE *f;
    struct mntent *mnt;

    if ((f = setmntent(MOUNTED, "r")) == NULL)
        return;

    while ((mnt = getmntent(f)) != NULL)
        if (strcmp(device_name, mnt->mnt_fsname) == 0)
            die("%s contains a mounted file system.", device_name);

    endmntent(f);
}

/* Establish the geometry and media parameters for the device */
static void establish_params(int device_num, int size)
{
    long loop_size;
    struct hd_geometry geometry;
    struct floppy_struct param;

    /* file image or floppy disk */
    if ((0 == device_num) || ((device_num & 0xff00) == 0x0200)) {
        if (0 == device_num) {
            param.size = size / 512;
            switch (param.size) {
                case 720:
                    param.sect = 9;
                    param.head = 2;
                    break;
                case 1440:
                    param.sect = 9;
                    param.head = 2;
                    break;
                case 2400:
                    param.sect = 15;
                    param.head = 2;
                    break;
                case 2880:
                    param.sect = 18;
                    param.head = 2;
                    break;
                case 5760:
                    param.sect = 36;
                    param.head = 2;
                    break;
                default:
                    /* fake values */
                    param.sect = 32;
                    param.head = 64;
                    break;
            }
        }
        /* is a floppy diskette */
        else {
            /*  Can we get the diskette geometry? */
            if (ioctl(dev, FDGETPRM, &param))
                die("unable to get diskette geometry for '%s'", device_name);
        }

        /*  Set up the geometry information */
        bs.sec_per_track = CT_LE_W(param.sect);
        bs.heads = CT_LE_W(param.head);

        /*  Set up the media descriptor byte */
        switch (param.size)	{
            /* 5.25", 2, 9, 40 - 360K */
            case 720:
                bs.media = (char)0xfd;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)112;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 9, 80 - 720K */
            case 1440:
                bs.media = (char)0xf9;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)112;
                bs.dir_entries[1] = (char)0;
                break;

            /* 5.25", 2, 15, 80 - 1200K */
            case 2400:
                bs.media = (char)0xf9;
                bs.sec_per_clus = (char)(atari_format ? 2 : 1);
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 36, 80 - 2880K */
            case 5760:
                bs.media = (char)0xf0;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 18, 80 - 1440K */
            case 2880:
floppy_default:
                bs.media = (char)0xf0;
                bs.sec_per_clus = (char)(atari_format ? 2 : 1);
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* Anything else */
            default:
                if (0 == device_num)
                    goto def_hd_params;
                else
                    goto floppy_default;
        }
    }
    /* This is a loop device */
    else if ((device_num & 0xff00) == 0x0700) {
        if (ioctl(dev, BLKGETSIZE, &loop_size))
            die("unable to get loop device size");

        /* Assuming the loop device -> floppy later */
        switch (loop_size) {
            /* 5.25", 2, 9, 40 - 360K */
            case 720:
                bs.sec_per_track = CF_LE_W(9);
                bs.heads = CF_LE_W(2);
                bs.media = (char)0xfd;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)112;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 9, 80 - 720K */
            case 1440:
                bs.sec_per_track = CF_LE_W(9);
                bs.heads = CF_LE_W(2);
                bs.media = (char)0xf9;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)112;
                bs.dir_entries[1] = (char)0;
                break;

            /* 5.25", 2, 15, 80 - 1200K */
            case 2400:
                bs.sec_per_track = CF_LE_W(15);
                bs.heads = CF_LE_W(2);
                bs.media = (char)0xf9;
                bs.sec_per_clus = (char)(atari_format ? 2 : 1);
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 36, 80 - 2880K */
            case 5760:
                bs.sec_per_track = CF_LE_W(36);
                bs.heads = CF_LE_W(2);
                bs.media = (char)0xf0;
                bs.sec_per_clus = (char)2;
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* 3.5", 2, 18, 80 - 1440K */
            case 2880:
                bs.sec_per_track = CF_LE_W(18);
                bs.heads = CF_LE_W(2);
                bs.media = (char)0xf0;
                bs.sec_per_clus = (char)(atari_format ? 2 : 1);
                bs.dir_entries[0] = (char)224;
                bs.dir_entries[1] = (char)0;
                break;

            /* Anything else: default hd setup */
            default:
                printf("Loop device does not match a floppy size, using "
                        "default hd params\n");
                bs.sec_per_track = CT_LE_W(32); /* these are fake values... */
                bs.heads = CT_LE_W(64);
                goto def_hd_params;
        }
    }
    /* Must be a hard disk then! */
    else {
        /* Can we get the drive geometry? (Note I'm not too sure about */
        /* whether to use HDIO_GETGEO or HDIO_REQ) */
        if (ioctl(dev, HDIO_GETGEO, &geometry) || geometry.sectors == 0
                || geometry.heads == 0) {
            printf("unable to get drive geometry, using default 255/63\n");
            bs.sec_per_track = CT_LE_W(63);
            bs.heads = CT_LE_W(255);
        }
        else {
            /* Set up the geometry information */
            bs.sec_per_track = CT_LE_W(geometry.sectors);
            bs.heads = CT_LE_W(geometry.heads);
        }
def_hd_params:
        bs.media = (char)0xf8; /* Set up the media descriptor for a hard drive */
        bs.dir_entries[0] = (char)0;	/* Default to 512 entries */
        bs.dir_entries[1] = (char)2;

        if (!fat_bits && blocks * SECTORS_PER_BLOCK > 1064960) {
            if (verbose)
                printf("Auto-selecting FAT32 for large filesystem\n");
            fat_bits = 32;
        }

        if (fat_bits == 32) {
            /* For FAT32, try to do the same as M$'s format command:
             * fs size < 256M: 0.5k clusters
             * fs size <   8G: 4k clusters
             * fs size <  16G: 8k clusters
             * fs size >= 16G: 16k clusters
             */
            unsigned long sz_mb =
                (blocks + (1 << (20 - BLOCK_SIZE_BITS)) - 1) >>
                (20 - BLOCK_SIZE_BITS);
            bs.sec_per_clus = sz_mb >= 16 * 1024 ? 32 :
                sz_mb >=  8 * 1024 ? 16 :
                sz_mb >=       256 ?  8 : 1;
        }
        else {
            /* FAT12 and FAT16: start at 4 sectors per cluster */
            bs.sec_per_clus = (char)4;
        }
    }
}

/* Create the filesystem data tables */
static void setup_tables(void)
{
    unsigned num_sectors;
    unsigned cluster_count = 0, sec_per_fat;
    unsigned fatdata;			/* Sectors for FATs + data area */
    struct tm *ctime;
    struct volume_info *vi = (fat_bits == 32 ? &bs.fat32.vi : &bs.oldfat.vi);

    if (atari_format)
        /* On Atari, the first few bytes of the boot sector are assigned
         * differently: The jump code is only 2 bytes (and m68k machine code
         * :-), then 6 bytes filler (ignored), then 3 byte serial number. */
        memcpy(bs.system_id - 1, "mkdosf", 6);
    else
        strncpy((char *)bs.system_id, "mkdosfs", sizeof(bs.system_id));

    if (sectors_per_cluster)
        bs.sec_per_clus = (char)sectors_per_cluster;

    if (fat_bits == 32) {
        /* Under FAT32, the root dir is in a cluster chain, and this is
         * signalled by bs.dir_entries being 0. */
        bs.dir_entries[0] = bs.dir_entries[1] = (char)0;
        root_dir_entries = 0;
    }
    else if (root_dir_entries) {
        /* Override default from establish_params() */
        bs.dir_entries[0] = (char)(root_dir_entries & 0x00ff);
        bs.dir_entries[1] = (char)((root_dir_entries & 0xff00) >> 8);
    }
    else
        root_dir_entries = bs.dir_entries[0] + (bs.dir_entries[1] << 8);

    if (atari_format) {
        bs.system_id[5] = (unsigned char)(volume_id & 0x000000ff);
        bs.system_id[6] = (unsigned char)((volume_id & 0x0000ff00) >> 8);
        bs.system_id[7] = (unsigned char)((volume_id & 0x00ff0000) >> 16);
    }
    else {
        vi->volume_id[0] = (unsigned char)(volume_id & 0x000000ff);
        vi->volume_id[1] = (unsigned char)((volume_id & 0x0000ff00) >> 8);
        vi->volume_id[2] = (unsigned char)((volume_id & 0x00ff0000) >> 16);
        vi->volume_id[3] = (unsigned char)(volume_id >> 24);
    }

    if (bs.media == 0xf8) {
        vi->drive_number = HD_DRIVE_NUMBER;  /* Set bios drive number to 80h */
    }
    else {
        vi->drive_number = FD_DRIVE_NUMBER;  /* Set bios drive number to 00h */
    }

    if (!atari_format) {
        memcpy(vi->label, volume_name, LEN_VOLUME_LABEL);

        memcpy(bs.boot_jump, dummy_boot_jump, 3);
        /* Patch in the correct offset to the boot code */
        bs.boot_jump[1] = ((fat_bits == 32 ?
                    (char *)&bs.fat32.boot_code :
                    (char *)&bs.oldfat.boot_code) - (char *)&bs) - 2;

        if (fat_bits == 32) {
            int offset = (char *)&bs.fat32.boot_code -
                (char *)&bs + MESSAGE_OFFSET + 0x7c00;

            if (dummy_boot_code[BOOTCODE_FAT32_SIZE - 1])
                printf("Warning: message too long; truncated\n");

            dummy_boot_code[BOOTCODE_FAT32_SIZE - 1] = 0;
            memcpy(bs.fat32.boot_code, dummy_boot_code, BOOTCODE_FAT32_SIZE);
            bs.fat32.boot_code[MSG_OFFSET_OFFSET] = offset & 0xff;
            bs.fat32.boot_code[MSG_OFFSET_OFFSET + 1] = offset >> 8;
        }
        else {
            memcpy(bs.oldfat.boot_code, dummy_boot_code, BOOTCODE_SIZE);
        }
        bs.boot_sign = CT_LE_W(BOOT_SIGN);
    }
    else {
        memcpy(bs.boot_jump, dummy_boot_jump_m68k, 2);
    }

    if (verbose >= 2)
        printf("Boot jump code is %02x %02x\n",
                bs.boot_jump[0], bs.boot_jump[1]);

    if (!reserved_sectors)
        reserved_sectors = (fat_bits == 32) ? 32 : 1;
    else {
        if (fat_bits == 32 && reserved_sectors < 2)
            die("On FAT32 at least 2 reserved sectors are needed.");
    }

    bs.reserved_cnt = CT_LE_W(reserved_sectors);

    if (verbose >= 2)
        printf("Using %d reserved sectors\n", reserved_sectors);

    bs.nfats = (char) nr_fats;
    if (!atari_format || fat_bits == 32)
        bs.hidden = bs.sec_per_track;
    else {
        /* In Atari format, hidden is a 16 bit field */
        __u16 hidden = CT_LE_W(hidden_sectors);
        if (hidden_sectors & ~0xffff)
            die("#hidden doesn't fit in 16bit field of Atari format\n");

        memcpy(&bs.hidden, &hidden, 2);
    }

    num_sectors = (long long)blocks * BLOCK_SIZE / sector_size;
    if (!atari_format) {
        unsigned sec_per_fat12, sec_per_fat16, sec_per_fat32;
        unsigned maxclust12, maxclust16, maxclust32;
        unsigned clust12, clust16, clust32;
        int maxclustsize;

        fatdata = num_sectors - cdiv(root_dir_entries * 32, sector_size) -
            reserved_sectors;

        if (sectors_per_cluster)
            bs.sec_per_clus = maxclustsize = sectors_per_cluster;
        else
            /* An initial guess for bs.sec_per_clus should already be set */
            maxclustsize = 128;

        if (verbose >= 2)
            printf("%d sectors for FAT+data, starting with %d sectors/cluster\n",
                    fatdata, bs.sec_per_clus);

        do {
            if (verbose >= 2)
                printf("Trying with %d sectors/cluster:\n", bs.sec_per_clus);

            /* The factor 2 below avoids cut-off errors for nr_fats == 1.
             * The "nr_fats*3" is for the reserved first two FAT entries */
            clust12 = 2 * ((long long)fatdata * sector_size + nr_fats * 3) /
                (2 * (int)bs.sec_per_clus * sector_size + nr_fats * 3);
            sec_per_fat12 = cdiv(((clust12 + 2) * 3 + 1) >> 1, sector_size);

            /* Need to recalculate number of clusters, since the unused parts of the
             * FATS and data area together could make up space for an additional,
             * not really present cluster. */
            clust12 = (fatdata - nr_fats * sec_per_fat12) / bs.sec_per_clus;
            maxclust12 = (sec_per_fat12 * 2 * sector_size) / 3;
            if (maxclust12 > MAX_CLUST_12)
                maxclust12 = MAX_CLUST_12;

            if (verbose >= 2)
                printf("FAT12: #clu=%u, fatlen=%u, maxclu=%u, limit=%u\n",
                        clust12, sec_per_fat12, maxclust12, MAX_CLUST_12);

            if (clust12 > maxclust12 - 2) {
                clust12 = 0;
                if (verbose >= 2)
                    printf("FAT12: too much clusters\n");
            }

            clust16 = ((long long)fatdata * sector_size + nr_fats * 4) /
                ((int)bs.sec_per_clus * sector_size + nr_fats * 2);
            sec_per_fat16 = cdiv((clust16 + 2) * 2, sector_size);

            /* Need to recalculate number of clusters, since the unused parts of the
             * FATS and data area together could make up space for an additional,
             * not really present cluster. */
            clust16 = (fatdata - nr_fats * sec_per_fat16) / bs.sec_per_clus;
            maxclust16 = (sec_per_fat16 * sector_size) / 2;
            if (maxclust16 > MAX_CLUST_16)
                maxclust16 = MAX_CLUST_16;

            if (verbose >= 2)
                printf("FAT16: #clu=%u, fatlen=%u, maxclu=%u, limit=%u\n",
                        clust16, sec_per_fat16, maxclust16, MAX_CLUST_16);

            if (clust16 > maxclust16 - 2) {
                if (verbose >= 2)
                    printf("FAT16: too much clusters\n");
                clust16 = 0;
            }

            /* The < 4078 avoids that the filesystem will be misdetected as having a
             * 12 bit FAT. */
            if (clust16 < FAT12_THRESHOLD && !(size_fat_by_user && fat_bits == 16)) {
                if (verbose >= 2)
                    printf("FAT16: would be misdetected as FAT12\n");
                clust16 = 0;
            }

            clust32 = ((long long)fatdata * sector_size + nr_fats * 8) /
                ((int)bs.sec_per_clus * sector_size + nr_fats * 4);
            sec_per_fat32 = cdiv((clust32 + 2) * 4, sector_size);

            /* Need to recalculate number of clusters, since the unused parts of the
             * FATS and data area together could make up space for an additional,
             * not really present cluster. */
            clust32 = (fatdata - nr_fats * sec_per_fat32) / bs.sec_per_clus;
            maxclust32 = (sec_per_fat32 * sector_size) / 4;
            if (maxclust32 > MAX_CLUST_32)
                maxclust32 = MAX_CLUST_32;

            if (clust32 && clust32 < MIN_CLUST_32 &&
                    !(size_fat_by_user && fat_bits == 32)) {
                clust32 = 0;
                if (verbose >= 2)
                    printf("FAT32: not enough clusters (%d)\n", MIN_CLUST_32);
            }

            if (verbose >= 2)
                printf("FAT32: #clu=%u, fatlen=%u, maxclu=%u, limit=%u\n",
                        clust32, sec_per_fat32, maxclust32, MAX_CLUST_32);

            if (clust32 > maxclust32) {
                clust32 = 0;
                if (verbose >= 2)
                    printf("FAT32: too much clusters\n");
            }

            if ((clust12 && (fat_bits == 0 || fat_bits == 12)) ||
                    (clust16 && (fat_bits == 0 || fat_bits == 16)) ||
                    (clust32 && fat_bits == 32))
                break;

            bs.sec_per_clus <<= 1;
        } while (bs.sec_per_clus && bs.sec_per_clus <= maxclustsize);

        /* Use the optimal FAT size if not specified;
         * FAT32 is (not yet) choosen automatically */
        if (!fat_bits) {
            fat_bits = (clust16 > clust12) ? 16 : 12;
            if (verbose >= 2)
                printf("Choosing %d bits for FAT\n", fat_bits);
        }

        switch (fat_bits) {
            case 12:
                cluster_count = clust12;
                sec_per_fat = sec_per_fat12;
                bs.sec_per_fat = CT_LE_W(sec_per_fat12);
                memcpy(vi->fs_type, MSDOS_FAT12_SIGN, 8);
                break;
            case 16:
                if (clust16 < FAT12_THRESHOLD) {
                    if (size_fat_by_user) {
                        fprintf(stderr, "WARNING: Not enough clusters for a "
                                "16 bit FAT! The filesystem will be\n"
                                "misinterpreted as having a 12 bit FAT without "
                                "mount option \"fat=16\".\n");
                    }
                    else {
                        fprintf(stderr, "This filesystem has an unfortunate size. "
                                "A 12 bit FAT cannot provide\n"
                                "enough clusters, but a 16 bit FAT takes up a little "
                                "bit more space so that\n"
                                "the total number of clusters becomes less than the "
                                "threshold value for\n"
                                "distinction between 12 and 16 bit FATs.\n");
                        die("Make the file system a bit smaller manually.");
                    }
                }
                cluster_count = clust16;
                sec_per_fat = sec_per_fat16;
                bs.sec_per_fat = CT_LE_W(sec_per_fat16);
                memcpy(vi->fs_type, MSDOS_FAT16_SIGN, 8);
                break;
            case 32:
                cluster_count = clust32;
                sec_per_fat = sec_per_fat32;
                bs.sec_per_fat = CT_LE_W(0);
                bs.fat32.sec_per_fat32 = CT_LE_L(sec_per_fat32);
                memcpy(vi->fs_type, MSDOS_FAT32_SIGN, 8);
                break;
            default:
                die("FAT not 12, 16 or 32 bits");
        }
    }
    else {
        unsigned clusters, maxclust;

        /* GEMDOS always uses a 12 bit FAT on floppies, and always a 16 bit FAT on
         * hard disks. So use 12 bit if the size of the file system suggests that
         * this fs is for a floppy disk, if the user hasn't explicitly requested a
         * size.
         */
        if (!fat_bits)
            fat_bits = (num_sectors == 1440 || num_sectors == 2400 ||
                    num_sectors == 2880 || num_sectors == 5760) ? 12 : 16;
        if (verbose >= 2)
            printf("Choosing %d bits for FAT\n", fat_bits);

        /* Atari format: cluster size should be 2, except explicitly requested by
         * the user, since GEMDOS doesn't like other cluster sizes very much.
         * Instead, tune the sector size for the FS to fit.
         */
        bs.sec_per_clus = sectors_per_cluster ? sectors_per_cluster : 2;
        if (!sector_size_set) {
            while (num_sectors > GEMDOS_MAX_SECTORS) {
                num_sectors >>= 1;
                sector_size <<= 1;
            }
        }

        if (verbose >= 2)
            printf("Sector size must be %d to have less than %d log. sectors\n",
                    sector_size, GEMDOS_MAX_SECTORS);

        /* Check if there are enough FAT indices for how much clusters we have */
        do {
            fatdata = num_sectors - cdiv(root_dir_entries * 32, sector_size) -
                reserved_sectors;

            /* The factor 2 below avoids cut-off errors for nr_fats == 1 and
             * fat_bits == 12
             * The "2*nr_fats*fat_bits/8" is for the reserved first two FAT entries
             */
            clusters =
                (2 * ((long long)fatdata * sector_size - 2 * nr_fats * fat_bits / 8)) /
                (2 * ((int)bs.sec_per_clus * sector_size + nr_fats * fat_bits / 8));
            sec_per_fat = cdiv((clusters + 2) * fat_bits / 8, sector_size);

            /* Need to recalculate number of clusters, since the unused parts of the
             * FATS and data area together could make up space for an additional,
             * not really present cluster. */
            clusters = (fatdata - nr_fats * sec_per_fat) / bs.sec_per_clus;
            maxclust = (sec_per_fat * sector_size * 8) / fat_bits;
            if (verbose >= 2)
                printf("ss=%d: #clu=%d, fat_len=%d, maxclu=%d\n",
                        sector_size, clusters, sec_per_fat, maxclust);

            /* last 10 cluster numbers are special (except FAT32: 4 high bits rsvd);
             * first two numbers are reserved */
            if (maxclust <= (fat_bits == 32 ? MAX_CLUST_32 : (1 << fat_bits) - 0x10) &&
                    clusters <= maxclust - 2)
                break;

            if (verbose >= 2)
                printf(clusters > maxclust - 2 ?
                        "Too many clusters\n" : "FAT too big\n");

            /* need to increment sector_size once more to  */
            if (sector_size_set)
                die("With this sector size, "
                        "the maximum number of FAT entries would be exceeded.");

            num_sectors >>= 1;
            sector_size <<= 1;
        } while (sector_size <= GEMDOS_MAX_SECTOR_SIZE);

        if (sector_size > GEMDOS_MAX_SECTOR_SIZE)
            die( "Would need a sector size > 16k, which GEMDOS can't work with");

        cluster_count = clusters;
        if (fat_bits != 32)
            bs.sec_per_fat = CT_LE_W(sec_per_fat);
        else {
            bs.sec_per_fat = 0;
            bs.fat32.sec_per_fat32 = CT_LE_L(sec_per_fat);
        }
    }

    fat_size = sec_per_fat * sector_size;
    fat_start = reserved_sectors * sector_size;

    bs.sector_size[0] = (char) (sector_size & 0x00ff);
    bs.sector_size[1] = (char) ((sector_size & 0xff00) >> 8);

    if (fat_bits == 32) {
        /* set up additional FAT32 fields */
        bs.fat32.flags = CT_LE_W(0);
        bs.fat32.version[0] = 0;
        bs.fat32.version[1] = 0;
        bs.fat32.root_cluster = CT_LE_L(2);
        bs.fat32.info_sector = CT_LE_W(1);

        if (!backup_boot)
            backup_boot = (reserved_sectors >= 7) ? 6 :
                (reserved_sectors >= 2) ? reserved_sectors - 1 : 0;
        else {
            if (backup_boot == 1)
                die("Backup boot sector must be after sector 1");
            else if (backup_boot >= reserved_sectors)
                die("Backup boot sector must be a reserved sector");
        }

        if (verbose >= 2)
            printf("Using sector %d as backup boot sector (0 = none)\n",
                    backup_boot);
        bs.fat32.backup_boot = CT_LE_W(backup_boot);
        memset(&bs.fat32.reserved2, 0, sizeof(bs.fat32.reserved2));
    }

    if (atari_format) {
        /* Just some consistency checks */
        if (num_sectors >= GEMDOS_MAX_SECTORS)
            die("GEMDOS can't handle more than 65531 sectors");
        else if (num_sectors >= OLDGEMDOS_MAX_SECTORS)
            printf("Warning: More than 32765 sector need TOS 1.04 "
                    "or higher.\n");
    }

    if (num_sectors >= 65536) {
        bs.sectors[0] = (char)0;
        bs.sectors[1] = (char)0;
        bs.total_sect = CT_LE_L(num_sectors);
    }
    else {
        bs.sectors[0] = (char)(num_sectors & 0x00ff);
        bs.sectors[1] = (char)((num_sectors & 0xff00) >> 8);
        if (!atari_format)
            bs.total_sect = CT_LE_L(0);
    }

    if (!atari_format)
        vi->extended_sig = MSDOS_EXT_SIGN;

    if (!cluster_count) {
        /* If yes, die if we'd spec'd sectors per cluster */
        if (sectors_per_cluster)
            die("Too many clusters for file system - try more sectors per cluster");
        else
            die("Attempting to create a too large file system");
    }

    /* The two following vars are in hard sectors, i.e. 512 byte sectors! */
    start_data_sector = (reserved_sectors + nr_fats * sec_per_fat) *
        (sector_size / HARD_SECTOR_SIZE);
    start_data_block = (start_data_sector + SECTORS_PER_BLOCK - 1) /
        SECTORS_PER_BLOCK;

    if (blocks < start_data_block + 32)	/* Arbitrary undersize file system! */
        die("Too few blocks for viable file system");

    if (verbose) {
        printf("%s has %d head%s and %d sector%s per track,\n",
                device_name, CF_LE_W(bs.heads), (CF_LE_W(bs.heads) != 1) ?
                "s" : "",
                CF_LE_W(bs.sec_per_track), (CF_LE_W(bs.sec_per_track) != 1) ?
                "s" : "");
        printf("logical sector size is %d,\n", sector_size);
        printf("using 0x%02x media descriptor, with %d sectors;\n",
                (int)(bs.media), num_sectors);
        printf("file system has %d %d-bit FAT%s and %d sector%s per cluster.\n",
                (int)(bs.nfats), fat_bits, (bs.nfats != 1) ? "s" : "",
                (int)(bs.sec_per_clus), (bs.sec_per_clus != 1) ? "s" : "");
        printf ("FAT size is %d sector%s, and provides %d cluster%s.\n",
                sec_per_fat, (sec_per_fat != 1) ? "s" : "",
                cluster_count, (cluster_count != 1) ? "s" : "");

        if (fat_bits != 32)
            printf ("Root directory contains %d slots.\n",
                    (int)(bs.dir_entries[0]) + (int)(bs.dir_entries[1]) * 256);

        printf ("Volume ID is %08lx, ", volume_id &
                (atari_format ? 0x00ffffff : 0xffffffff));

        if (strcmp(volume_name, LABEL_NONAME))
            printf("volume label %s.\n", volume_name);
        else
            printf("no volume label.\n");
    }

    if (fat_bits != 32) {
        /* Make the file allocation tables! */
        if ((fat = (unsigned char *)alloc_mem(sec_per_fat * sector_size)) == NULL)
            die("unable to allocate space for FAT image in memory");

        memset(fat, 0, sec_per_fat * sector_size);

        fat[0] = (unsigned char)bs.media;  /* Put media type in first byte! */
    }
    else {
        int i, j;

        if ((fat = (unsigned char *)alloc_mem(sector_size)) == NULL)
            die("unable to allocate space for FAT image in memory");

        memset(fat, 0, sector_size);

        seekto(reserved_sectors * sector_size, "first FAT");
        for (i = 1; i <= nr_fats; i++) {
            for (j = 0; j < sec_per_fat; j++) {
                writebuf(fat, sector_size, "FAT");
            }
        }
        mark_FAT_cluster(0, 0x0fffff00 | bs.media);	/* Initial fat entries */
    }

    mark_FAT_cluster(1, 0x0fffffff);
    if (fat_bits == 32) {
        /* Mark cluster 2 as EOF (used for root dir) */
        mark_FAT_cluster(2, VALUE_FAT_EOF);
    }

    /* Make the root directory entries */
    size_root_dir = (fat_bits == 32) ?
        bs.sec_per_clus * sector_size :
        (((int)bs.dir_entries[1] * 256 + (int)bs.dir_entries[0]) *
         sizeof(struct dir_entry));

    if ((root_dir = (struct dir_entry *)alloc_mem(size_root_dir)) == NULL) {
        free_mem(fat);		/* Tidy up before we die! */
        die("unable to allocate space for root directory in memory");
    }

    memset(root_dir, 0, size_root_dir);
    if (memcmp(volume_name, LABEL_NONAME, LEN_VOLUME_LABEL)) {
        struct dir_entry *de = &root_dir[0];

        memcpy(de->name, volume_name, LEN_VOLUME_LABEL);
        de->attr = ATTR_VOLUME;
        ctime = localtime(&create_time);
        de->time = CT_LE_W((unsigned short)((ctime->tm_sec >> 1) +
                    (ctime->tm_min << 5) + (ctime->tm_hour << 11)));
        de->date = CT_LE_W((unsigned short)(ctime->tm_mday +
                    ((ctime->tm_mon + 1) << 5) +
                    ((ctime->tm_year - 80) << 9)));
        de->ctime_ms = 0;
        de->ctime = de->time;
        de->cdate = de->date;
        de->adate = de->date;
        de->starthi = CT_LE_W(0);
        de->start = CT_LE_W(0);
        de->size = CT_LE_L(0);
    }

    if (fat_bits == 32) {
        /* For FAT32, create an fsinfo sector */
        struct fsinfo_sector *info;

        if (!(fsinfo = alloc_mem(sector_size)))
            die("Out of memory");

        memset(fsinfo, 0, sector_size);
        /* fsinfo structure is at offset 0x1e0 in info sector by observation */
        info = (struct fsinfo_sector *)fsinfo;

        /* Magic for fsinfo structure */
        info->magic = CT_LE_L(LEAD_SIGN);
        info->signature = CT_LE_L(STRUCT_SIGN);

        /* We've allocated cluster 2 for the root dir. */
        info->free_clusters = CT_LE_L(cluster_count - 1);
        info->next_cluster = CT_LE_L(2);

        /* Info sector also must have boot sign */
        info->boot_sign = CT_LE_W(BOOT_SIGN);
    }

    if (!(blank_sector = alloc_mem(sector_size)))
        die("Out of memory");

    memset(blank_sector, 0, sector_size);
}

/* Write the new filesystem's data tables to wherever they're going to end up! */


static void write_tables(void)
{
    int x;
    int sec_per_fat;

    sec_per_fat = (fat_bits == 32) ?
        CF_LE_L(bs.fat32.sec_per_fat32) : CF_LE_W(bs.sec_per_fat);

    seekto(0, "start of device");

    /* clear all reserved sectors */
    for (x = 0; x < reserved_sectors; ++x)
        writebuf(blank_sector, sector_size, "reserved sector");

    /* seek back to sector 0 and write the boot sector */
    seekto(0, "boot sector");
    writebuf((char *)&bs, sizeof(struct boot_sector), "boot sector");

    /* on FAT32, write the info sector and backup boot sector */
    if (fat_bits == 32) {
        seekto(CF_LE_W(bs.fat32.info_sector) * sector_size, "info sector");
        writebuf(fsinfo, 512, "info sector");

        if (backup_boot != 0) {
            seekto(backup_boot * sector_size, "backup boot sector");
            writebuf((char *)&bs, sizeof(struct boot_sector),
                    "backup boot sector");
        }
    }

    /* seek to start of FATS and write them all */
    if (fat_bits != 32) {
        seekto(reserved_sectors * sector_size, "first FAT");
        for (x = 1; x <= nr_fats; x++)
            writebuf(fat, sec_per_fat * sector_size, "FAT");
    } else {
        seekto(start_data_sector * sector_size, "root directory");
    }

    /* Write the root directory directly after the last FAT. This is the root
     * dir area on FAT12/16, and the first cluster on FAT32. */
    writebuf((char *)root_dir, size_root_dir, "root directory");

    if (use_template == 1) {
        /* dupe template into reserved sectors */
        seekto(0, "Start of partition");

        if (fat_bits == 32) {
            writebuf(template_boot_code, 3, "backup jmpBoot");
            seekto(0x5a, "sector 1 boot area");
            writebuf(template_boot_code + 0x5a, 420, "sector 1 boot area");
            seekto(512 * 2, "third sector");

            if (backup_boot != 0) {
                writebuf(template_boot_code + 512 * 2,
                        backup_boot * sector_size - 512 * 2,
                        "data to backup boot");
                seekto(backup_boot * sector_size, "backup boot sector");
                writebuf(template_boot_code, 3, "backup jmpBoot");
                seekto(backup_boot * sector_size + 0x5a,
                        "backup boot sector boot area");
                writebuf(template_boot_code + 0x5a, 420,
                        "backup boot sector boot area");
                seekto((backup_boot + 2) * sector_size,
                        "sector following backup code");
                writebuf(template_boot_code + (backup_boot + 2) * sector_size,
                        (reserved_sectors - backup_boot - 2) * 512,
                        "remaining data");
            } else {
                writebuf(template_boot_code + 512 * 2,
                        (reserved_sectors - 2) * 512, "remaining data");
            }
        } else {
            writebuf(template_boot_code, 3, "jmpBoot");
            seekto(0x3e, "sector 1 boot area");
            writebuf(template_boot_code + 0x3e, 448, "boot code");
        }
    }

    if (blank_sector)
        free_mem(blank_sector);

    if (fsinfo)
        free_mem(fsinfo);

    free_mem(root_dir);   /* Free up the root directory space from setup_tables */
    free_mem(fat);  /* Free up the fat table space reserved during setup_tables */

    if (fsync(dev) < 0) {
        error("Error: fsync failed");
    }
}

/* Report the command usage and return a failure error code */
void usage(void)
{
    fatal_error("\
            Usage: mkdosfs [-A] [-c] [-C] [-v] [-I] [-l bad-block-file] [-b backup-boot-sector]\n\
            [-m boot-msg-file] [-n volume-name] [-i volume-id] [-B bootcode]\n\
            [-s sectors-per-cluster] [-S logical-sector-size] [-f number-of-FATs]\n\
            [-h hidden-sectors] [-F fat-size] [-r root-dir-entries] [-R reserved-sectors]\n\
            /dev/name [blocks]\n");
}

/* The "main" entry point into the utility - we pick up the options
 * and attempt to process them in some sort of sensible way.
 * In the event that some/all of the options are invalid
 * we need to tell the user so that something can be done! */
int main(int argc, char **argv)
{
    int c;
    char *tmp;
    char *listfile = NULL;
    FILE *msgfile;
    struct stat statbuf;
    int i = 0, pos, ch;
    int create = 0;
    unsigned long long cblocks = 0;
    int min_sector_size;

    if (argc && *argv) {		/* What's the program name? */
        char *p;
        program_name = *argv;
        if ((p = strrchr(program_name, '/')))
            program_name = p + 1;
    }

    time(&create_time);
    volume_id = (long)create_time;	/* Default volume ID = creation time */
    check_atari(&atari_format);

    printf("%s " VERSION " (" VERSION_DATE ")\n", program_name);

    while ((c = getopt(argc, argv, "AB:b:cCf:F:Ii:l:m:n:r:R:s:S:h:v")) != EOF) {
        /* Scan the command line for options */
        switch (c) {
            case 'A':		/* toggle Atari format */
                atari_format = !atari_format;
                break;
            case 'b':		/* b : location of backup boot sector */
                backup_boot = (int)strtol(optarg, &tmp, 0);
                if (*tmp || backup_boot < 2 || backup_boot > 0xffff) {
                    printf("Bad location for backup boot sector : %s\n", optarg);
                    usage();
                }
                break;
            case 'c':		/* c : Check FS as we build it */
                check = TRUE;
                break;
            case 'C':		/* C : Create a new file */
                create = TRUE;
                break;
            case 'f':		/* f : Choose number of FATs */
                nr_fats = (int)strtol(optarg, &tmp, 0);
                if (*tmp || nr_fats < 1 || nr_fats > 4) {
                    printf("Bad number of FATs : %s\n", optarg);
                    usage();
                }
                break;
            case 'F':		/* F : Choose FAT size */
                fat_bits = (int)strtol(optarg, &tmp, 0);
                if (*tmp ||
                        (fat_bits != 12 && fat_bits != 16 && fat_bits != 32)) {
                    printf("Bad FAT type : %s\n", optarg);
                    usage();
                }
                size_fat_by_user = 1;
                break;
            case 'h':        /* h : number of hidden sectors */
                hidden_sectors = (int)strtol(optarg, &tmp, 0);
                if (*tmp || hidden_sectors < 0) {
                    printf("Bad number of hidden sectors : %s\n", optarg);
                    usage();
                }
                break;
            case 'I':
                ignore_full_disk = 1;
                break;
            case 'i':		/* i : specify volume ID */
                volume_id = strtoul(optarg, &tmp, 16);
                if (*tmp) {
                    printf("Volume ID must be a hexadecimal number\n");
                    usage();
                }
                break;
            case 'l':		/* l : Bad block filename */
                listfile = optarg;
                break;
            case 'B':         /* B : read in bootcode */
                if (strcmp(optarg, "-")) {
                    msgfile = fopen(optarg, "r");
                    if (!msgfile)
                        perror(optarg);
                }
                else
                    msgfile = stdin;

                if (msgfile) {
                    if (!(template_boot_code = alloc_mem(MAX_RESERVED)))
                        die("Out of memory");

                    /* The template boot sector including reserved
                     * must not be > 65535 */
                    use_template = 1;
                    i = 0;
                    do {
                        ch = getc(msgfile);
                        switch (ch) {
                            case EOF:
                                break;
                            default:
                                template_boot_code[i++] = ch; /* Store character */
                                break;
                        }
                    } while (ch != EOF && i < MAX_RESERVED);

                    ch = getc(msgfile); /* find out if we're at EOF */

                    /* Fill up with zeros */
                    while (i < MAX_RESERVED)
                        template_boot_code[i++] = '\0';

                    if (ch != EOF)
                        printf("Warning: template too long; truncated after %d bytes\n", i);

                    if (msgfile != stdin)
                        fclose(msgfile);
                }
                break;
            case 'm':		/* m : Set boot message */
                if (strcmp(optarg, "-")) {
                    msgfile = fopen(optarg, "r");
                    if (!msgfile)
                        perror(optarg);
                }
                else
                    msgfile = stdin;

                if (msgfile) {
                    /* The boot code ends at offset 448 and needs a null terminator */
                    i = MESSAGE_OFFSET;
                    pos = 0;		/* We are at beginning of line */
                    do {
                        ch = getc(msgfile);
                        switch (ch) {
                            case '\r':	/* Ignore CRs */
                            case '\0':	/* and nulls */
                                break;
                            case '\n':	/* LF -> CR+LF if necessary */
                                if (pos) {  /* If not at beginning of line */
                                    dummy_boot_code[i++] = '\r';
                                    pos = 0;
                                }
                                dummy_boot_code[i++] = '\n';
                                break;
                            case '\t':	/* Expand tabs */
                                do {
                                    dummy_boot_code[i++] = ' ';
                                    pos++;
                                } while (pos % 8 && i < BOOTCODE_SIZE - 1);
                                break;
                            case EOF:
                                dummy_boot_code[i++] = '\0'; /* Null terminator */
                                break;
                            default:
                                dummy_boot_code[i++] = ch; /* Store character */
                                pos++;	/* Advance position */
                                break;
                        }
                    } while (ch != EOF && i < BOOTCODE_SIZE - 1);

                    /* Fill up with zeros */
                    while (i < BOOTCODE_SIZE - 1)
                        dummy_boot_code[i++] = '\0';

                    dummy_boot_code[BOOTCODE_SIZE - 1] = '\0'; /* Just in case */

                    if (ch != EOF)
                        printf("Warning: message too long; truncated\n");

                    if (msgfile != stdin)
                        fclose(msgfile);
                }
                break;
            case 'n':		/* n : Volume name */
                sprintf(volume_name, "%-11.11s", optarg);
                break;
            case 'r':		/* r : Root directory entries */
                root_dir_entries = (int)strtol(optarg, &tmp, 0);
                if (*tmp ||
                        root_dir_entries < 16 || root_dir_entries > 32768) {
                    printf("Bad number of root directory entries : %s\n", optarg);
                    usage();
                }
                break;
            case 'R':		/* R : number of reserved sectors */
                reserved_sectors = (int)strtol(optarg, &tmp, 0);
                if (*tmp || reserved_sectors < 1 || reserved_sectors > 0xffff) {
                    printf("Bad number of reserved sectors : %s\n", optarg);
                    usage();
                }
                break;
            case 's':		/* s : Sectors per cluster */
                sectors_per_cluster = (int)strtol(optarg, &tmp, 0);
                if (*tmp || (sectors_per_cluster != 1 && sectors_per_cluster != 2
                            && sectors_per_cluster != 4 && sectors_per_cluster != 8
                            && sectors_per_cluster != 16 && sectors_per_cluster != 32
                            && sectors_per_cluster != 64 && sectors_per_cluster != 128))
                {
                    printf("Bad number of sectors per cluster : %s\n", optarg);
                    usage();
                }
                break;
            case 'S':		/* S : Sector size */
                sector_size = (int)strtol(optarg, &tmp, 0);
                if (*tmp || (sector_size != 512 && sector_size != 1024 &&
                            sector_size != 2048 && sector_size != 4096 &&
                            sector_size != 8192 && sector_size != 16384 &&
                            sector_size != 32768))
                {
                    printf("Bad logical sector size : %s\n", optarg);
                    usage();
                }
                sector_size_set = 1;
                break;
            case 'v':		/* v : Verbose execution */
                ++verbose;
                break;
            default:
                printf("Unknown option: %c\n", c);
                usage();
        }
    }

    if (optind < argc) {
        device_name = argv[optind];  /* Determine the number of blocks in the FS */

        if (!device_name) {
            printf("No device specified.\n");
            usage();
        }

        if (!create)
            cblocks = count_blocks(device_name); /*  Have a look and see! */
    }

    if (optind == argc - 2)	{ /*  Either check the user specified number */
        blocks = strtoull(argv[optind + 1], &tmp, 0);
        if (!create && blocks != cblocks) {
            fprintf(stderr, "Warning: block count mismatch: ");
            fprintf(stderr, "found %llu but assuming %llu.\n", cblocks, blocks);
        }
    }
    else if (optind == argc - 1) {  /*  Or use value found */
        if (create)
            die("Need intended size with -C.");
        blocks = cblocks;
        tmp = "";
    }
    else {
        fprintf(stderr, "No device specified!\n");
        usage();
    }

    if (*tmp) {
        printf("Bad block count : %s\n", argv[optind + 1]);
        usage();
    }

    if (check && listfile)	/* Auto and specified bad block handling are mutually */
        die("-c and -l are incompatible");		/* exclusive of each other! */

    if (!create) {
        /* Is the device already mounted? */
        check_mount(device_name);

        /* Is it a suitable device to build the FS on? */
        dev = open(device_name, O_EXCL | O_RDWR);
        if (dev < 0)
            die("unable to open %s", device_name);
    }
    else {
        off_t offset = blocks * BLOCK_SIZE - 1;
        char null = 0;
        /* create the file */
        dev = open(device_name, O_EXCL | O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (dev < 0)
            die("unable to create %s", device_name);

        /* seek to the intended end-1, and write one byte. this creates a
         * sparse-as-possible file of appropriate size. */
        if (llseek(dev, offset, SEEK_SET) != offset)
            die("seek failed");

        if (write(dev, &null, 1) < 0)
            die("write failed");

        if (llseek(dev, 0, SEEK_SET) != 0)
            die("seek failed");
    }

    if (fstat(dev, &statbuf) < 0)
        die("unable to stat %s", device_name);

    if (!S_ISBLK(statbuf.st_mode)) {
        statbuf.st_rdev = 0;
        check = 0;
    }
    else
        /*
         * Ignore any 'full' fixed disk devices, if -I is not given.
         * On a MO-disk one doesn't need partitions.  The filesytem can go
         * directly to the whole disk.  Under other OSes this is known as
         * the 'superfloppy' format.  As I don't know how to find out if
         * this is a MO disk I introduce a -I (ignore) switch.  -Joey
         */
        if (!ignore_full_disk && (
                    (statbuf.st_rdev & 0xff3f) == 0x0300 || /* hda, hdb */
                    (statbuf.st_rdev & 0xff0f) == 0x0800 || /* sd */
                    (statbuf.st_rdev & 0xff3f) == 0x0d00 || /* xd */
                    (statbuf.st_rdev & 0xff3f) == 0x1600)  /* hdc, hdd */
           )
		die("Will not try to make filesystem on full-disk device '%s'"
				"(use -I if wanted)", device_name);

    if (sector_size_set) {
        if (ioctl(dev, BLKSSZGET, &min_sector_size) >= 0)
            if (sector_size < min_sector_size) {
                sector_size = min_sector_size;
                fprintf(stderr, "Warning: sector size was set to %d (minimal for this device)\n", sector_size);
            }
    }
    else {
        if (ioctl(dev, BLKSSZGET, &min_sector_size) >= 0) {
            sector_size = min_sector_size;
            sector_size_set = 1;
        }
    }

    if (sector_size > 4096)
        fprintf(stderr,
                "Warning: sector size is set to %d > 4096, "
                "such filesystem will not propably mount\n",
                sector_size);

    /* Establish the media parameters */
    establish_params(statbuf.st_rdev, statbuf.st_size);
    setup_tables();		/* Establish the file system tables */

    if (check)			/* Determine any bad block locations and mark them */
        check_blocks();
    else if (listfile)
        get_list_blocks(listfile);

    print_mem();
    write_tables();		/* Write the file system tables away! */

    close(dev);
    exit(0);            /* Terminate with no errors! */
}

/* That's All Folks */
/* Local Variables: */
/* tab-width: 8     */
/* End:             */
