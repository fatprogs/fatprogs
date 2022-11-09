/* check.h  -  Check and repair a PC/MS-DOS file system */

/* Written 1993 by Werner Almesberger */

#ifndef _CHECK_H
#define _CHECK_H

/* Allocate a free slot in the root directory for a new file. The file name is
   constructed after 'pattern', which must include a %d type format for printf
   and expand to exactly 11 characters. The name actually used is written into
   the 'de' structure, the rest of *de is cleared. The offset returned is to
   where in the filesystem the entry belongs. */
loff_t alloc_rootdir_entry(DOS_FS *fs, DIR_ENT *de, const char *pattern);

/* Scans the root directory and recurses into all subdirectories. See check.c
   for all the details. Returns a non-zero integer if the file system has to
   be checked again. */
int scan_root(DOS_FS *fs);

int check_volume_label(DOS_FS *fs);
int check_valid_label(char *label);
void scan_root_only(DOS_FS *fs, label_t **head, label_t **last);
void write_label(DOS_FS *fs, char *label, label_t **head, label_t **last);

int check_dirty_flag(DOS_FS *fs);
void clean_dirty_flag(DOS_FS *fs);

#endif
