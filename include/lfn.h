/* SPDX-License-Identifier : GPL-2.0 */

/* lfn.h  -  Functions for handling VFAT long filenames */

/* Written 1998 by Roman Hodek */

#ifndef _LFN_H
#define _LFN_H

/* Reset the state of the LFN parser. */
void lfn_reset(void);

/* Process a dir slot that is a VFAT LFN entry. */
void lfn_add_slot(DIR_ENT *de, loff_t dir_offset);

/* Retrieve the long name for the proper dir entry. */
char *lfn_get(DIR_ENT *de);

void lfn_check_orphaned(void);

void lfn_remove(void);
int lfn_exist(void);
void scan_lfn(DIR_ENT *de, loff_t offset);

#endif
