/* SPDX-License-Identifier : GPL-2.0 */

/* fat.h  -  Read/write access to the FAT */

/* Written 1993 by Werner Almesberger */

#ifndef _FAT_H
#define _FAT_H

typedef enum fat_select {
    FAT_NONE = -1,
    FAT_FIRST = 0,
    FAT_SECOND = 1,
} fat_select_t;

/* Loads the FAT of the file system described by FS. Initializes the FAT,
   replaces broken FATs and rejects invalid cluster entries. */
void read_fat(DOS_FS *fs);

void get_fat_entry(DOS_FS *fs, uint32_t cluster, uint32_t *value, void *fat);
void get_fat(DOS_FS *fs, uint32_t cluster, uint32_t *value);
void modify_fat(DOS_FS *fs, uint32_t cluster, uint32_t new);

void set_bitmap_reclaim(DOS_FS *fs, uint32_t cluster);
void clear_bitmap_reclaim(DOS_FS *fs, uint32_t cluster);

void set_bitmap_occupied(DOS_FS *fs, uint32_t cluster);
void clear_bitmap_occupied(DOS_FS *fs, uint32_t cluster);

void init_alloc_cluster(void);
void inc_alloc_cluster(void);
void dec_alloc_cluster(void);

/* Changes the value of the CLUSTERth cluster of the FAT of FS to NEW. Special
   values of NEW are -1 (EOF, 0xff8 or 0xfff8) and -2 (bad sector, 0xff7 or
   0xfff7) */
void set_fat(DOS_FS *fs, uint32_t cluster, uint32_t new);
void set_fat_immed(DOS_FS *fs, uint32_t cluster, uint32_t new);

/* Returns a non-zero integer if the CLUSTERth cluster is marked as bad or zero
   otherwise. */
int bad_cluster(DOS_FS *fs, uint32_t cluster);

/* Returns the number of the cluster following CLUSTER, or -1 if this is the
   last cluster of the respective cluster chain. CLUSTER must not be a bad
   cluster. */
uint32_t next_cluster(DOS_FS *fs, uint32_t cluster);

/* same as next_cluster except that next_cluster exit when next cluster is bad,
 * this function does not exit even if next cluster is bad,
 * just return bad value */
uint32_t __next_cluster(DOS_FS *fs, uint32_t cluster);

/* Returns the byte offset of CLUSTER, relative to the respective device. */
loff_t cluster_start(DOS_FS *fs, uint32_t cluster);

/* Scans the disk for currently unused bad clusters and marks them as bad. */
void fix_bad(DOS_FS *fs);

/* Marks all allocated, but unused clusters as free. */
void reclaim_free(DOS_FS *fs);

/* Scans the FAT for chains of allocated, but unused clusters and creates files
   for them in the root directory. Also tries to fix all inconsistencies (e.g.
   loops, shared clusters, etc.) in the process. */
void reclaim_file(DOS_FS *fs);

/* Updates free cluster count in FSINFO sector. */
uint32_t update_free(DOS_FS *fs);

#endif
