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

extern int interactive, list, verbose, test, write_immed;
extern int atari_format;
extern unsigned n_files;
extern void *mem_queue;

#endif

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
