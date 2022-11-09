/* dosfsck.h  -  Common data structures and global variables */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#ifndef _DOSFSCK_H
#define _DOSFSCK_H

#include <sys/types.h>
#define _LINUX_STAT_H		/* hack to avoid inclusion of <linux/stat.h> */
#define _LINUX_STRING_H_	/* hack to avoid inclusion of <linux/string.h>*/
#define _LINUX_FS_H             /* hack to avoid inclusion of <linux/fs.h> */

#include <stdint.h>
#include <asm/types.h>

#include "dosfs.h"

#define VFAT_LN_ATTR (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_VOLUME)
#define VFAT_LN_ATTR_MASK \
    (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_VOLUME | ATTR_DIR | ATTR_ARCH)

#define IS_LFN_ENT(attr) (((attr) & VFAT_LN_ATTR_MASK) == VFAT_LN_ATTR)
#define IS_VOLUME_LABEL(attr) ((attr) & ATTR_VOLUME)

#define FAT32_DIRTY_BIT_MASK    0x8000000
#define FAT16_DIRTY_BIT_MASK    0x8000

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
    uint32_t value;
    uint32_t reserved;
    DOS_FILE *owner;
    uint32_t prev;      /* number of clusters that point this cluster */
} FAT_ENTRY;

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
    FAT_ENTRY *fat;
    char *label;
} DOS_FS;

typedef enum { LABEL_FLAG_NONE, LABEL_FLAG_BAD } label_flag_t;
#define LABEL_FAKE_ADDR     (label_t **)(0xFFBADA00)

/* struct _label */
typedef struct _label {
    int flag;
    DOS_FILE *file;
    struct _label *next;
} label_t;

label_t *label_head;
label_t *label_last;

#ifndef offsetof
#define offsetof(t, e)	((off_t)&(((t *)0)->e))
#endif

extern int interactive, list, verbose, test, write_immed;
extern int atari_format;
extern unsigned n_files;
extern void *mem_queue;

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

#endif

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
