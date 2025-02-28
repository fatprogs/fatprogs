/* SPDX-License-Identifier : GPL-2.0 */

/* io.h  -  Virtual disk input/output */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#ifndef _IO_H
#define _IO_H

#include <sys/types.h> /* for loff_t */
#include <sys/mman.h>

/* In earlier versions, an own llseek() was used, but glibc lseek() is
 * sufficient (or even better :) for 64 bit offsets in the meantime */
#define llseek lseek

/* Opens the file system PATH. If RW is zero, the file system is opened
   read-only, otherwise, it is opened read-write. */
void fs_open(char *path, int rw);

void fs_find_data_copy(loff_t pos, int size, void *data);

/* Reads SIZE bytes starting at POS into DATA. Performs all applicable
   changes. */
void fs_read(loff_t pos, int size, void *data);

/* Returns a non-zero integer if SIZE bytes starting at POS can be read without
   errors. Otherwise, it returns zero. */
int fs_test(loff_t pos, int size);

/* If write_immed is non-zero, SIZE bytes are written from DATA to the disk,
   starting at POS. If write_immed is zero, the change is added to a list in
   memory. */
void fs_write(loff_t pos, int size, void *data);
void fs_write_immed(loff_t pos, int size, void *data);

int fs_flush(int write);
/* Closes the file system, performs all pending changes if WRITE is non-zero
   and removes the list of changes. Returns a non-zero integer if the file
   system has been changed since the last fs_open, zero otherwise. */
void fs_close(void);

/* Determines whether the file system has changed. See fs_close. */
int fs_changed(void);

void *fs_mmap(void *hint, off_t offset, size_t length);
int fs_munmap(void *addr, size_t length);

/* Print wrong data in CHNAGE lists */
void print_changes(void);

/* Major number of device (0 if file) and size (in 512 byte sectors) */
extern unsigned device_no;

#endif
