/* SPDX-License-Identifier : GPL-2.0 */

#ifndef _DOSFS_H_
#define _DOSFS_H_

#include <linux/msdos_fs.h>
#include <stdint.h>
//#include <asm/types.h>

#undef CF_LE_W
#undef CF_LE_L
#undef CT_LE_W
#undef CT_LE_L

#if __BYTE_ORDER == __BIG_ENDIAN
#include <byteswap.h>
#define CF_LE_W(v) bswap_16(v)
#define CF_LE_L(v) bswap_32(v)
#define CT_LE_W(v) CF_LE_W(v)
#define CT_LE_L(v) CF_LE_L(v)
#else
#define CF_LE_W(v) (v)
#define CF_LE_L(v) (v)
#define CT_LE_W(v) (v)
#define CT_LE_L(v) (v)
#endif /* __BIG_ENDIAN */

#if defined __alpha || defined __ia64__ || defined __s390x__ || defined __x86_64__ || defined __ppc64__
/* Unaligned fields must first be copied byte-wise */
#define GET_UNALIGNED_W(f)			\
    ({						\
     unsigned short __v;			\
     memcpy(&__v, &f, sizeof(__v));	\
     CF_LE_W(*(unsigned short *)&__v);	\
     })
#else
#define GET_UNALIGNED_W(f) CF_LE_W(*(unsigned short *)&f)
#endif

#define MSDOS_EXT_SIGN 0x29 /* extended boot sector signature */
#define MSDOS_FAT12_SIGN "FAT12   "	/* FAT12 filesystem signature */
#define MSDOS_FAT16_SIGN "FAT16   "	/* FAT16 filesystem signature */
#define MSDOS_FAT32_SIGN "FAT32   "	/* FAT32 filesystem signature */

#define BOOT_SIGN       (0xAA55)        /* trail signature */
#define LEAD_SIGN       (0x41615252)    /* fsinfo lead signature('RRaA') */
#define STRUCT_SIGN     (0x61417272)    /* fsinfo struct signature('rrAa') */

#define BOOTCODE_SIZE		448 /* 0x1c0 */
#define BOOTCODE_FAT32_SIZE	420 /* 0x1A4 */

#define LABEL_NONAME    "NO NAME    "
#define LABEL_EMPTY     "           "
#define LEN_VOLUME_LABEL    11
#define LEN_FILE_NAME       LEN_VOLUME_LABEL  /* total 8.3 file size(8 + 3) */
#define LEN_FILE_BASE   8
#define LEN_FILE_EXT    3   /* file extension size */

#define VFAT_LN_ATTR (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_VOLUME)
#define VFAT_LN_ATTR_MASK \
    (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_VOLUME | ATTR_DIR | ATTR_ARCH)
#define VFAT_ATTR_MASK  (ATTR_DIR | ATTR_VOLUME)

/* define attribute check macro according to FAT specification document */
#define IS_LFN_ENT(attr)    (((attr) & VFAT_LN_ATTR_MASK) == VFAT_LN_ATTR)
#define IS_VOLUME_LABEL(attr)   (((attr) & VFAT_ATTR_MASK) == ATTR_VOLUME)
#define IS_DIR(attr)        (((attr) & VFAT_ATTR_MASK) == ATTR_DIR)
#define IS_FILE(attr)       (((attr) & VFAT_ATTR_MASK) == 0)

extern int atari_format;
extern uint32_t max_clus_num;

/* value to use as end-of-file marker */
#define FAT_EOF(fs)	((atari_format ? 0xfff : 0xff8) | FAT_EXTD(fs))
#define FAT_IS_EOF(fs, v) ((uint32_t)(v) >= (0xff8 | FAT_EXTD(fs)))
/* value to mark bad clusters */
#define FAT_BAD(fs)	(0xff7 | FAT_EXTD(fs))
/* range of values used for bad clusters */
#define FAT_MIN_BAD(fs)	((atari_format ? 0xff0 : 0xff7) | FAT_EXTD(fs))
#define FAT_MAX_BAD(fs)	((atari_format ? 0xff7 : 0xff7) | FAT_EXTD(fs))
#define FAT_IS_BAD(fs, v) ((v) >= FAT_MIN_BAD(fs) && (v) <= FAT_MAX_BAD(fs))

/* return -16 as a number with fs->fat_bits bits */
#define FAT_EXTD(fs)	(((1 << fs->eff_fat_bits) - 1) & ~0xf)

#define FAT32_DIRTY_BIT_MASK    0x8000000
#define FAT16_DIRTY_BIT_MASK    0x8000

/* __attribute__ ((packed)) is used on all structures to make gcc ignore any
 * alignments */
struct volume_info {
    __u8    drive_number;   /* BIOS drive number */
    __u8    state;          /* Undocumented, but used for mount state */
    __u8    extended_sig;   /* Extended Signature:
                               0x29 if fields below exist (DOS 3.3+) */
    __u8    volume_id[4];   /* Volume ID number */
    __u8    label[LEN_VOLUME_LABEL];   /* Volume label */
    __u8    fs_type[8];     /* Typically FAT12 or FAT16 */
} __attribute__ ((packed));

struct boot_sector {
    __u8    boot_jump[3];   /* Boot strap short or near jump */
    __u8    system_id[8];   /* Name - can be used to special case
                                    partition manager volumes */
    __u8    sector_size[2]; /* bytes per logical sector */
    __u8    sec_per_clus;   /* number of sectors per cluster */
    __u16   reserved_cnt;   /* number of reserved sectors */
    __u8    nfats;          /* number of FATs */
    __u8    dir_entries[2]; /* root directory entries */
    __u8    sectors[2];	    /* number of total sectors */
    __u8    media;          /* media code (unused) */
    __u16   sec_per_fat;    /* number of sectors per FAT */
    __u16   sec_per_track;  /* number of sectors per track */
    __u16   heads;          /* number of heads */
    __u32   hidden;         /* hidden sectors (one track) */
    __u32   total_sect;	    /* # of total sectors
                               (if sectors == 0, FAT32) */
    union {
        struct {
            struct volume_info vi;
            __u8    boot_code[BOOTCODE_SIZE];
        } __attribute__ ((packed)) _oldfat;
        struct {
            __u32   sec_per_fat32;  /* # of sectors per FAT (FAT32) */
            __u16   flags;          /* bit 8: fat mirroring, low 4: active fat */
            __u8    version[2];     /* major, minor filesystem version */
            __u32   root_cluster;   /* first cluster in root directory */
            __u16   info_sector;    /* filesystem info sector */
            __u16   backup_boot;    /* backup boot sector */
            __u16   reserved2[6];   /* Unused */
            struct volume_info vi;
            __u8    boot_code[BOOTCODE_FAT32_SIZE];
        } __attribute__ ((packed)) _fat32;
    } __attribute__ ((packed)) fstype;
    __u16       boot_sign;
} __attribute__ ((packed));
#define fat32	fstype._fat32
#define oldfat	fstype._oldfat

struct fsinfo_sector {
    __u32   magic;      /* Magic for info sector ('RRaA' - 0x41615252) */
    __u8    junk[0x1dc];
    __u32   reserved1;  /* Nothing as far as I can tell */
    __u32   signature;  /* Magic for info sector ('rrAa' - 0x61417272) */
    __u32   free_clusters;  /* Free cluster count.  -1 if unknown */
    __u32   next_cluster;   /* Most recently allocated cluster. */
    __u32   reserved2[3];
    __u16   reserved3;
    __u16   boot_sign;
} __attribute__ ((packed));

struct dir_entry {
    __u8    name[LEN_FILE_NAME]; /* name and extension */
    __u8    attr;       /* attribute bits */
    __u8    lcase;      /* Case for base and extension */
    __u8    ctime_ms;   /* Creation time, milliseconds */
    __u16   ctime;      /* Creation time */
    __u16   cdate;      /* Creation date */
    __u16   adate;      /* Last access date */
    __u16   starthi;    /* High 16 bits of cluster in FAT32 */
    __u16   time, date, start;  /* time, date and first cluster */
    __u32   size;       /* file size (in bytes) */
} __attribute__ ((packed));

typedef struct dir_entry DIR_ENT;

typedef struct _dos_file {
    DIR_ENT dir_ent;
    char *lfn;
    loff_t offset;
    struct _dos_file *parent; /* parent directory */
    struct _dos_file *next; /* next entry */
    struct _dos_file *first; /* first entry (directory only) */
} DOS_FILE;

typedef struct {
    uint32_t start; /* start cluster number of fat cache */
    uint32_t cnt;       /* # of clusters in fat cache */
    uint32_t first_cpc; /* # of clusters in first fat cache */
    uint32_t last_cpc;  /* # of clusters in last fat cache */
    uint32_t cpc;       /* clusters per cache - # of clusters per fat cache */
    uint32_t diff;      /* diff from fat start and mmap aligned address */
    char *addr;
} FAT_CACHE;

typedef struct {
    int nfats;
    loff_t fat_start;
    unsigned int fat_size; /* unit is bytes */
    unsigned int fat_bits; /* size of a FAT entry */
    unsigned int eff_fat_bits; /* # of used bits in a FAT entry */
    unsigned int fat_state; /* state of filesystem */
    uint32_t root_cluster; /* 0 for old-style root dir */
    loff_t root_start;
    unsigned int root_entries;
    loff_t data_start;
    unsigned int cluster_size;
    uint32_t clusters;  /* total number of data area clusters */
    loff_t fsinfo_start; /* 0 if not present */
    uint32_t free_clusters;
    loff_t backupboot_start; /* 0 if not present */
    unsigned int bitmap_size;
    unsigned long *bitmap;  /* for marked cluster on disk */
    unsigned long *real_bitmap; /* for real cluster chain through scan */
    unsigned long *reclaim_bitmap;  /* for orphan cluster reclaiming */
    FAT_CACHE fat_cache;
    char *label;
} DOS_FS;

#endif /* _DOSFS_H_ */
