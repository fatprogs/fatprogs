/* SPDX-License-Identifier : GPL-2.0 */

/* check.c  -  Check and repair a PC/MS-DOS file system */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "fat.h"
#include "file.h"
#include "lfn.h"
#include "check.h"

void remove_lfn(DOS_FS *fs, DOS_FILE *file);
void scan_volume_entry(DOS_FS *fs, label_t **head, label_t **last);

static int check_dots(DOS_FS *fs, DOS_FILE *parent, int dots);
static int add_dot_entries(DOS_FS *fs, DOS_FILE *parent, int dots);

static DOS_FILE *root;

#define DOT_ENTRY       0
#define DOTDOT_ENTRY    1

/* get start field of a dir entry */
#define FSTART(p, fs) \
    ((uint32_t)CF_LE_W(p->dir_ent.start) | \
     (fs->fat_bits == 32 ? CF_LE_W(p->dir_ent.starthi) << 16 : 0))

#define MODIFY(p, i, v)					\
    do {							\
        if (p->offset) {					\
            p->dir_ent.i = v;				\
            fs_write(p->offset + offsetof(DIR_ENT, i),		\
                    sizeof(p->dir_ent.i), &p->dir_ent.i);	\
        }							\
    } while(0)

#define MODIFY_START(p, v, fs)						\
    do {									\
        uint32_t __v = (v);						\
        if (!p->offset) {							\
            /* writing to fake entry for FAT32 root dir */			\
            if (!__v)                                   \
                die("Oops, deleting FAT32 root dir!");		\
            fs->root_cluster = __v;						\
            p->dir_ent.start = CT_LE_W(__v&0xffff);				\
            p->dir_ent.starthi = CT_LE_W(__v >> 16);				\
            __v = CT_LE_L(__v);						\
            fs_write((loff_t)offsetof(struct boot_sector, fat32.root_cluster),	\
                    sizeof(((struct boot_sector *)0)->fat32.root_cluster),	\
                    &__v);							\
        }									\
        else {								\
            MODIFY(p, start, CT_LE_W((__v)&0xffff));				\
            if (fs->fat_bits == 32)						\
                MODIFY(p, starthi, CT_LE_W((__v) >> 16));			\
        }									\
    } while(0)


loff_t alloc_rootdir_entry(DOS_FS *fs, DIR_ENT *de, const char *pattern)
{
    static int curr_num = 0;
    loff_t offset;

    if (fs->root_cluster) {
        DIR_ENT d2;
        int i = 0, got = 0;
        uint32_t clu_num, prev = 0;
        loff_t offset2;

        /* TODO: use 'next_cluster' field of 'struct info_sector' */
        clu_num = fs->root_cluster;
        offset = cluster_start(fs, clu_num);

        /* find empty slot */
        while (clu_num > 0 && clu_num != -1) {
            fs_read(offset, sizeof(DIR_ENT), &d2);
            if (IS_FREE(d2.name) && d2.attr != VFAT_LN_ATTR) {
                got = 1;
                break;
            }

            i += sizeof(DIR_ENT);
            offset += sizeof(DIR_ENT);
            if ((i % fs->cluster_size) == 0) {
                prev = clu_num;
                if ((clu_num = next_cluster(fs, clu_num)) == 0 || clu_num == -1)
                    break;
                offset = cluster_start(fs, clu_num);
            }
        }

        if (!got) {
            /* no free slot, need to extend root dir: alloc next free cluster
             * after previous one */
            if (!prev)
                die("Root directory has no cluster allocated!");

            /* find free cluster */
            for (clu_num = prev + 1; clu_num != prev; clu_num++) {
                if (clu_num >= fs->clusters + 2)
                    clu_num = 2;
                if (!fs->fat[clu_num].value)
                    break;
            }

            if (clu_num == prev)
                die("Root directory full and no free cluster");

            set_fat(fs, prev, clu_num);
            set_fat(fs, clu_num, -1);
            set_owner(fs, clu_num, get_owner(fs, fs->root_cluster));

            /* clear new cluster */
            memset(&d2, 0, sizeof(d2));
            offset = cluster_start(fs, clu_num);

            /* TODO: ??? why it use for loop to clear cluster */
            for (i = 0; i < fs->cluster_size; i += sizeof(DIR_ENT))
                fs_write(offset + i, sizeof(d2), &d2);
        }

        memset(de, 0, sizeof(DIR_ENT));

        /* if pattern is NULL, then just allocate root entry
         * and do not fill DIR_ENT structure */
        if (!pattern)
            return offset;

        /* make entry using pattern */
        while (1) {
            char expanded[12];

            sprintf(expanded, pattern, curr_num);
            memcpy(de->name, expanded, LEN_FILE_NAME);
            clu_num = fs->root_cluster;
            i = 0;
            offset2 = cluster_start(fs, clu_num);

            while (clu_num > 0 && clu_num != -1) {
                fs_read(offset2, sizeof(DIR_ENT), &d2);

                /* check duplicated entry */
                if (offset2 != offset &&
                        !strncmp((char *)d2.name, (char *)de->name, MSDOS_NAME))
                    break;

                i += sizeof(DIR_ENT);
                offset2 += sizeof(DIR_ENT);

                if ((i % fs->cluster_size) == 0) {
                    if ((clu_num = next_cluster(fs, clu_num)) == 0 ||
                            clu_num == -1)
                        break;
                    offset2 = cluster_start(fs, clu_num);
                }
            }

            if (clu_num == 0 || clu_num == -1)
                break;

            if (++curr_num >= 10000)
                die("Unable to create unique name");
        }
    }
    else {
        DIR_ENT *root_ent;
        int next_free = 0, scan;

        root_ent = alloc(fs->root_entries * sizeof(DIR_ENT));
        fs_read(fs->root_start, fs->root_entries * sizeof(DIR_ENT), root_ent);

        while (next_free < fs->root_entries) {
            if (IS_FREE(root_ent[next_free].name) &&
                    root_ent[next_free].attr != VFAT_LN_ATTR)
                break;
            else
                next_free++;
        }

        if (next_free == fs->root_entries)
            die("Root directory is full.");

        offset = fs->root_start + next_free * sizeof(DIR_ENT);
        memset(de, 0, sizeof(DIR_ENT));

        /* if pattern is NULL, then just allocate root entry
         * and do not fill DIR_ENT structure */
        if (!pattern)
            return offset;

        /* make entry using pattern */
        while (1) {
            char expanded[12];

            sprintf(expanded, pattern, curr_num);
            memcpy(de->name, expanded, LEN_FILE_NAME);

            for (scan = 0; scan < fs->root_entries; scan++)
                if (scan != next_free &&
                        !strncmp((char *)root_ent[scan].name,
                            (char *)de->name, MSDOS_NAME))
                    break;
            if (scan == fs->root_entries)
                break;

            if (++curr_num >= 10000)
                die("Unable to create unique name");
        }
        free(root_ent);
    }
    ++n_files;
    return offset;
}

static char *path_name(DOS_FILE *file)
{
    static char path[PATH_MAX * 2];

    if (!file)
        *path = 0;
    else {
        if (strlen(path_name(file->parent)) > PATH_MAX)
            die("Path name too long.");
        if (strcmp(path, "/") != 0)
            strcat(path, "/");

        strcpy(strrchr(path, 0), file->lfn ?
                file->lfn : file_name(file->dir_ent.name));
    }
    return path;
}

/* Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec */
static int day_n[] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0, 0
};

/* Convert a MS-DOS time/date pair to a UNIX date (seconds since 1 1 70). */
time_t date_dos2unix(unsigned short time, unsigned short date)
{
    int month,year;
    time_t secs;

    month = ((date >> 5) & 15) - 1;
    year = date >> 9;
    secs = (time & 31) * 2 + 60 * ((time >> 5) & 63) + (time >> 11) * 3600 + 86400 *
        ((date & 31) - 1 + day_n[month] + (year / 4) + year * 365 - ((year & 3) == 0 &&
            month < 2 ? 1 : 0) + 3653);
    /* days since 1.1.70 plus 80's leap day */
    return secs;
}

static char *file_stat(DOS_FILE *file)
{
    static char temp[256];
    struct tm *tm;
    char tmp[128];
    time_t date;

    date = date_dos2unix(CF_LE_W(file->dir_ent.time),
            CF_LE_W(file->dir_ent.date));
    tm = localtime(&date);
    strftime(tmp, 127, "%H:%M:%S %b %d %Y", tm);
    sprintf(temp,"  Size %u bytes, date %s", CF_LE_L(file->dir_ent.size), tmp);

    return temp;
}

/* illegal characters in short file name:
 * - less than 0x20 except for the special case of 0x05 in dir_ent.name[0].
 * - 0x22("), 0x2A(*), 0x2B(+), 0x2C(,), 0x2E(.), 0x2F(/),
 *   0x3A(:), 0x3B(;), 0x3C(<), 0x3D(=), 0x3E(>), 0x3F(?),
 *   0x5B([), 0x5C(\), 0x5D(]),
 *   0x7C(|)
 *
 * legal characters in long file name:
 * - 0x2B(+), 0x2c(,), 0x3B(;), 0x3D(=), 0x5B([), 0x5D(])
 * - 0x2E(.) can be used multiple times within long file name
 *   leading and embedded periods are allowed.
 *   trailing periods are ignored.
 * - 0x20( ) is valid character in a long file name
 *   embedded spaces within long file name are allowed.
 *   leading and trailing spaces are ignored.
 */
static int bad_name(unsigned char *name)
{
    int i, spc, suspicious = 0;
    char *bad_chars = atari_format ? "*?\\/:" : "*?<>|\"\\/:";

    /* Do not complain about (and auto-correct) the extended attribute files
     * of OS/2. */
    if (strncmp((char *)name, "EA DATA  SF", LEN_FILE_NAME) == 0 ||
            strncmp((char *)name, "WP ROOT  SF", LEN_FILE_NAME) == 0)
        return 0;

    /* check file body characters */
    for (i = 0; i < LEN_FILE_BASE; i++) {
        if (name[i] < ' ' || name[i] == 0x7f)
            return 1;
        if (name[i] > 0x7f)
            ++suspicious;
        if (strchr(bad_chars, name[i]))
            return 1;
    }

    /* check file extension characters */
    for (i = LEN_FILE_BASE; i < LEN_FILE_NAME; i++) {
        if (name[i] < ' ' || name[i] == 0x7f)
            return 1;
        if (name[i] > 0x7f)
            ++suspicious;
        if (strchr(bad_chars, name[i]))
            return 1;
    }

    /* check space character in file body characters */
    spc = 0;
    for (i = 0; i < LEN_FILE_BASE; i++) {
        if (name[i] == ' ')
            spc = 1;
        else if (spc)
            /* non-space after a space not allowed, space terminates the name
             * part */
            return 1;
    }

    /* check space character in file extension characters */
    spc = 0;
    for (i = LEN_FILE_BASE; i < LEN_FILE_NAME; i++) {
        if (name[i] == ' ')
            spc = 1;
        else if (spc)
            /* non-space after a space not allowed, space terminates the name
             * part */
            return 1;
    }

    /* Under GEMDOS, chars >= 128 are never allowed. */
    if (atari_format && suspicious)
        return 1;

    /* Only complain about too much suspicious chars in interactive mode,
     * never correct them automatically. The chars are all basically ok, so we
     * shouldn't auto-correct such names. */
    if (interactive && suspicious > 6)
        return 1;
    return 0;
}

static void drop_file(DOS_FS *fs, DOS_FILE *file)
{
    uint32_t cluster;
    int skip_set_owner = 0;

    /* when drop ".", ".." entry, should not change onwer of those entries */
    if (strncmp((char *)file->dir_ent.name, MSDOS_DOT, LEN_FILE_NAME) ||
            strncmp((char *)file->dir_ent.name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
        skip_set_owner = 1;
    }

    remove_lfn(fs, file);
    MODIFY(file, name[0], DELETED_FLAG);
    if (!skip_set_owner) {
        for (cluster = FSTART(file, fs);
                cluster > 0 && cluster < fs->clusters + FAT_START_ENT;
                cluster = next_cluster(fs, cluster)) {
            set_owner(fs, cluster, NULL);
        }
    }
    --n_files;
}

static void truncate_file(DOS_FS *fs, DOS_FILE *file, uint32_t clusters)
{
    int deleting;
    uint32_t walk, next;

    walk = FSTART(file, fs);
    if ((deleting = !clusters))
        MODIFY_START(file, 0, fs);

    while (walk > 0 && walk != -1) {
        next = next_cluster(fs, walk);
        if (deleting)
            set_fat(fs, walk, 0);
        else if ((deleting = !--clusters))
            set_fat(fs, walk, -1);

        walk = next;
    }
}

/* return 1 find file lfn name in parent entry,
 * else return 0 */
static int __find_lfn(DOS_FS *fs, DOS_FILE *parent, loff_t offset,
        DOS_FILE *file)
{
    DIR_ENT de;

    if (!offset) {
        return 0;
    }

    fs_read(offset, sizeof(DIR_ENT), &de);

    if (de.attr == VFAT_LN_ATTR && !IS_FREE(de.name)) {
        scan_lfn(&de, offset);
        return 0;
    }

    if (memcmp(de.name, file->dir_ent.name, LEN_FILE_NAME) == 0 &&
            offset == file->offset && lfn_exist()) {
        /* found */
        return 1;
    }

    lfn_reset();
    return 0;
}

static int find_lfn(DOS_FS *fs, DOS_FILE *parent, DOS_FILE *file)
{
    uint32_t clus_num;
    unsigned int clus_size;
    loff_t offset;
    DIR_ENT de;

    clus_num = FSTART(parent, fs);
    clus_size = fs->cluster_size;

    if (IS_LFN_ENT(file->dir_ent.attr)) {
        loff_t off = file->offset;

        while (clus_num > 0 && clus_num != -1) {
            fs_read(off, sizeof(DIR_ENT), &de);
            if (!IS_LFN_ENT(de.attr) && !IS_VOLUME_LABEL(de.attr)) {
                file->offset = off;
                memcpy(&file->dir_ent, &de, sizeof(DIR_ENT));
                break;
            }

            off += sizeof(DIR_ENT);
            off %= clus_size;
            if (!(off % clus_size))
                clus_num = next_cluster(fs, clus_num);
        }
    }

    offset = 0;
    while (clus_num > 0 && clus_num != -1) {
        if (__find_lfn(fs, parent, cluster_start(fs, clus_num) + offset, file))
            /* found */
            return 1;

        offset += sizeof(DIR_ENT);
        offset %= clus_size;
        if (!(offset % clus_size))
            clus_num = next_cluster(fs, clus_num);
    }

    return 0;
}

void remove_lfn(DOS_FS *fs, DOS_FILE *file)
{
    DOS_FILE *parent;
    int save_interactive;

    lfn_reset();
    parent = file->parent;
    if (!parent) {
        printf("Can't remove lfn of root entry\n");
        return;
    }

    /* find_lfn may change DOS_FILE *file data contents.
     * if file's attribute is LFN, then find_lfn will find DE,
     * and change it's offset and DE data to DOS_FILE *file */
    save_interactive = interactive;
    interactive = 0;
    if (find_lfn(fs, parent, file)) {
        lfn_remove();
    }
    interactive = save_interactive;
}

static void auto_rename(DOS_FS *fs, DOS_FILE *file)
{
    DOS_FILE *first, *walk;
    uint32_t number;
    char name[MSDOS_NAME + 1];

    if (!file->offset)
        return;	/* cannot rename FAT32 root dir */

    first = file->parent ? file->parent->first : root;
    number = 0;
    while (1) {
        snprintf(name, MSDOS_NAME + 1, "FSCK%04d%03d",
                number / 1000, number % 1000);
        memcpy(file->dir_ent.name, name, MSDOS_NAME);

        for (walk = first; walk; walk = walk->next)
            if (walk != file &&
                    !strncmp((char *)walk->dir_ent.name,
                        (char *)file->dir_ent.name, MSDOS_NAME)) {
                break;
            }

        if (!walk) {
            fs_write(file->offset, MSDOS_NAME, file->dir_ent.name);

            /* remove lfn related with previous name */
            remove_lfn(fs, file);
            file->lfn = NULL;
            return;
        }

        number++;
        if (number > 9999999) {
            die("Too many files need repair.");
        }
    }
    die("Can't generate a unique name.");
}

static void rename_file(DOS_FS *fs, DOS_FILE *file)
{
    unsigned char name[46];
    unsigned char *walk, *here;

    if (!file->offset) {
        printf("Cannot rename FAT32 root dir\n");
        return;	/* cannot rename FAT32 root dir */
    }

    while (1) {
        printf("New name: ");
        fflush(stdout);
        if (fgets((char *)name, 45, stdin)) {
            if ((here = (unsigned char *)strchr((char *)name,'\n')))
                *here = 0;

            for (walk = (unsigned char *)strrchr((char *)name, 0);
                    walk >= name && (*walk == ' ' || *walk == '\t');
                    walk--);

            walk[1] = 0;
            for (walk = name; *walk == ' ' || *walk == '\t'; walk++);

            if (file_cvt(walk, file->dir_ent.name)) {
                fs_write(file->offset, MSDOS_NAME, file->dir_ent.name);

                /* remove lfn related with previous name */
                remove_lfn(fs, file);
                file->lfn = NULL;
                return;
            }
        }
    }
}

/*
 * check lists in check_file().
 *
 * Directory:
 *  - entry has non-zero size
 *  - '.' entry point wrong entry
 *  - '..' entry point wrong parent entry
 *  - start cluster is wrong (0)
 *
 * Common:
 *  - start cluster number exceeds max limit
 *  - bad cluster (free or bad) check
 *  - compare entry size and cluster chain size
 *  - cluster duplication(shared cluster)
 *
 * set onwer field of DOS_FILE structure.
 *
 *  RETURN VALUE
 *  return 1 if need to restart, in case that already checked directory entry has
 *  been truncated. return 0 other cases.
 */
static int check_file(DOS_FS *fs, DOS_FILE *file)
{
    DOS_FILE *owner;
    int restart;
    uint32_t expect,
             curr,
             this,
             clusters,  /* num. of cluster occupied by 'file' */
             prev,
             walk,
             clusters2; /* num. of cluster occupied by previous entry
                           shared with same cluster */

    if (file->dir_ent.attr & ATTR_DIR) {
        if (CF_LE_L(file->dir_ent.size)) {
            printf("%s\n  Directory has non-zero size. Fixing it.\n",
                    path_name(file));
            MODIFY(file, size, CT_LE_L(0));
        }

        if (file->parent &&
                !strncmp((char *)file->dir_ent.name, MSDOS_DOT, MSDOS_NAME)) {
            expect = FSTART(file->parent, fs);
            if (FSTART(file, fs) != expect) {
                printf("%s\n  Start (%u) does not point to parent (%u)\n",
                        path_name(file), FSTART(file, fs), expect);
                MODIFY_START(file, expect, fs);
            }
            return 0;
        }

        if (file->parent &&
                !strncmp((char *)file->dir_ent.name, MSDOS_DOTDOT, MSDOS_NAME)) {
            expect = file->parent->parent ? FSTART(file->parent->parent, fs) : 0;
            if (fs->root_cluster && expect == fs->root_cluster)
                expect = 0;

            if (FSTART(file,fs) != expect) {
                printf("%s\n  Start (%u) does not point to .. (%u)\n",
                        path_name(file), FSTART(file, fs), expect);
                MODIFY_START(file, expect, fs);
            }
            return 0;
        }

        if (FSTART(file, fs) == 0) {
            printf ("%s\n  Start does point to root directory. Deleting dir. \n",
                    path_name(file));
            remove_lfn(fs, file);
            MODIFY(file, name[0], DELETED_FLAG);
            return 0;
        }
    }

    if (FSTART(file, fs) >= fs->clusters + 2) {
        if (file->dir_ent.attr & ATTR_DIR) {
            printf("%s\n  Directory start cluster beyond limit (%u > %u). "
                    "Deleting dir.\n",
                    path_name(file), FSTART(file, fs), fs->clusters + 1);
            remove_lfn(fs, file);
            MODIFY_START(file, 0, fs);
            MODIFY(file, name[0], DELETED_FLAG);
            return 0;
        }

        printf("%s\n  Start cluster beyond limit (%u > %u). Truncating file.\n",
                path_name(file), FSTART(file, fs), fs->clusters + 1);
        if (!file->offset)
            die("Bad FAT32 root directory! (bad start cluster)\n");
        MODIFY_START(file, 0, fs);
    }

    clusters = prev = 0;
    for (curr = FSTART(file, fs) ? FSTART(file, fs) : -1;
            curr != -1; curr = next_cluster(fs, curr)) {
        if (!fs->fat[curr].value || bad_cluster(fs, curr)) {
            printf("%s\n  Contains a %s cluster (%u). Assuming EOF.\n",
                    path_name(file), fs->fat[curr].value ? "bad" : "free", curr);
            if (prev)
                set_fat(fs, prev, -1);
            else if (!file->offset)
                die("FAT32 root dir starts with a bad cluster!");
            else
                MODIFY_START(file, 0, fs);
            break;
        }

        /* check duplicatedly?
         * is it better that check outside of 'for' loop?
         * and already same check routine is there except calling
         * truncate_file(). */
        if (!(file->dir_ent.attr & ATTR_DIR) && CF_LE_L(file->dir_ent.size) <=
                (unsigned long long)clusters * fs->cluster_size) {
            printf("%s\n  File size is %u bytes, cluster chain length is > %llu "
                    "bytes.\n  Truncating file to %u bytes.\n",
                    path_name(file),
                    CF_LE_L(file->dir_ent.size),
                    (unsigned long long)clusters * fs->cluster_size,
                    CF_LE_L(file->dir_ent.size));
            truncate_file(fs, file, clusters);
            break;
        }

        /* check shared clusters */
        if ((owner = get_owner(fs, curr))) {
            int do_trunc = 0;
            printf("%s  and\n", path_name(owner));
            printf("%s\n  share clusters.\n", path_name(file));
            clusters2 = 0;

            for (walk = FSTART(owner, fs); walk > 0 && walk != -1;
                    walk = next_cluster(fs, walk)) {
                if (walk == curr)
                    break;
                else
                    clusters2++;
            }

            restart = file->dir_ent.attr & ATTR_DIR;
            if (!owner->offset) {
                printf("  Truncating second to %llu bytes because first "
                        "is FAT32 root dir.\n",
                        (unsigned long long)clusters * fs->cluster_size);
                do_trunc = 2;
            }
            else if (!file->offset) {
                printf("  Truncating first to %llu bytes because second "
                        "is FAT32 root dir.\n",
                        (unsigned long long)clusters2 * fs->cluster_size);
                do_trunc = 1;
            }
            else if (interactive)
                printf("1) Truncate first to %llu bytes%s\n"
                        "2) Truncate second to %llu bytes\n",
                        (unsigned long long)clusters2 * fs->cluster_size,
                        restart ? " and restart" : "",
                        (unsigned long long)clusters * fs->cluster_size);
            else
                printf("  Truncating second to %llu bytes.\n",
                        (unsigned long long)clusters * fs->cluster_size);

            if (do_trunc != 2 &&
                    (do_trunc == 1 ||
                     (interactive && get_key("12", "?") == '1'))) {

                /* in case of truncating first entry */
                prev = 0;
                clusters2 = 0;
                for (this = FSTART(owner, fs);
                        this > 0 && this != -1;
                        this = next_cluster(fs, this)) {

                    if (this == curr) {
                        if (prev)
                            set_fat(fs, prev, -1);
                        else
                            MODIFY_START(owner, 0, fs);

                        MODIFY(owner, size,
                                CT_LE_L((unsigned long long)clusters2 *
                                    fs->cluster_size));
                        if (restart)
                            return 1;

                        while (this > 0 && this != -1) {
                            set_owner(fs, this, NULL);
                            this = next_cluster(fs, this);
                        }
                        this = curr;
                        break;
                    }
                    clusters2++;
                    prev = this;
                }

                if (this != curr)
                    die("Internal error: didn't find cluster %d in chain"
                            " starting at %d", curr, FSTART(owner, fs));
            }
            else {
                if (prev)
                    set_fat(fs, prev, -1);
                else
                    MODIFY_START(file, 0, fs);
                break;
            }
        }
        set_owner(fs, curr, file);
        clusters++;
        prev = curr;
    }

    if (!(file->dir_ent.attr & ATTR_DIR) && CF_LE_L(file->dir_ent.size) >
            (unsigned long long)clusters * fs->cluster_size) {

        printf("%s\n  File size is %u bytes, cluster chain length is %llu bytes."
                "\n  Truncating file to %llu bytes.\n",
                path_name(file), CF_LE_L(file->dir_ent.size),
                (unsigned long long)clusters * fs->cluster_size,
                (unsigned long long)clusters * fs->cluster_size);
        MODIFY(file, size,
                CT_LE_L((unsigned long long)clusters * fs->cluster_size));
    }

    return 0;
}

/* check all sibling entires */
static int check_files(DOS_FS *fs, DOS_FILE *start)
{
    while (start) {
        if (check_file(fs, start))
            return 1;
        start = start->next;
    }
    return 0;
}

/*
 * root : first entry of parent
 *
 * check bad name of sibling entires,
 * check duplicate entries,
 * check '.', '..' entries that is not directory
 * check '.', '..' entires missing (not support yet)
 */
static int check_dir(DOS_FS *fs, DOS_FILE **root, int dots)
{
    DOS_FILE *parent, **walk, **scan;
    int skip, redo;
    int good, bad;

    if (!*root)
        return 0;

    parent = (*root)->parent;
    good = bad = 0;
    for (walk = root; *walk; walk = &(*walk)->next) {
        if (bad_name((*walk)->dir_ent.name))
            bad++;
        else
            good++;
    }

    if (*root && parent && good + bad > 4 && bad > good / 2) {
        printf("%s\n  Has a large number of bad entries. (%d/%d)\n",
                path_name(parent), bad, good + bad);
        if (!dots)
            printf("  Not dropping root directory.\n");
        else if (!interactive)
            printf("  Not dropping it in auto-mode.\n");
        else if (get_key("yn", "Drop directory ? (y/n)") == 'y') {
            truncate_file(fs, parent, 0);
            MODIFY(parent, name[0], DELETED_FLAG);
            /* buglet: deleted directory stays in the list. */
            return 1;
        }
    }

    redo = 0;
    walk = root;
    while (*walk) {
        if (!((*walk)->dir_ent.attr & ATTR_VOLUME) &&
                bad_name((*walk)->dir_ent.name)) {

            printf("%s\n", path_name(*walk));
            printf("  Bad file name (%s).\n",
                    file_name((*walk)->dir_ent.name));

            if (interactive)
                printf("1) Drop file\n"
                        "2) Rename file\n"
                        "3) Auto-rename\n"
                        "4) Keep it\n");
            else
                printf("  Auto-renaming it.\n");

            switch (interactive ? get_key("1234", "?") : '3') {
                case '1':
                    drop_file(fs, *walk);
                    walk = &(*walk)->next;
                    continue;
                case '2':
                    rename_file(fs, *walk);
                    redo = 1;
                    break;
                case '3':
                    auto_rename(fs, *walk);
                    printf("  Renamed to %s\n",
                            file_name((*walk)->dir_ent.name));
                    break;
                case '4':
                    break;
            }
        }

        /* don't check for duplicates of the volume label */
        if (!((*walk)->dir_ent.attr & ATTR_VOLUME)) {
            scan = &(*walk)->next;
            skip = 0;
            while (*scan && !skip) {
                if (!((*scan)->dir_ent.attr & ATTR_VOLUME) &&
                        !strncmp((char *)((*walk)->dir_ent.name),
                            (char *)((*scan)->dir_ent.name), MSDOS_NAME)) {
                    printf("%s\n  Duplicate directory entry.\n  First  %s\n",
                            path_name(*walk), file_stat(*walk));
                    printf("  Second %s\n", file_stat(*scan));

                    if (interactive)
                        printf("1) Drop first\n"
                                "2) Drop second\n"
                                "3) Rename first\n"
                                "4) Rename second\n"
                                "5) Auto-rename first\n"
                                "6) Auto-rename second\n");
                    else
                        printf("  Auto-renaming second.\n");

                    switch (interactive ? get_key("123456", "?") : '6') {
                        case '1':
                            drop_file(fs, *walk);
                            *walk = (*walk)->next;
                            skip = 1;
                            break;
                        case '2':
                            drop_file(fs, *scan);
                            *scan = (*scan)->next;
                            continue;
                        case '3':
                            rename_file(fs, *walk);
                            printf("  Renamed to %s\n", path_name(*walk));
                            redo = 1;
                            break;
                        case '4':
                            rename_file(fs, *scan);
                            printf("  Renamed to %s\n", path_name(*walk));
                            redo = 1;
                            break;
                        case '5':
                            auto_rename(fs, *walk);
                            printf("  Renamed to %s\n",
                                    file_name((*walk)->dir_ent.name));
                            break;
                        case '6':
                            auto_rename(fs, *scan);
                            printf("  Renamed to %s\n",
                                    file_name((*scan)->dir_ent.name));
                            break;
                    }
                }
                scan = &(*scan)->next;
            }

            if (skip)
                continue;
        }

        if (!redo)
            walk = &(*walk)->next;
        else {
            walk = root;
            redo = 0;
        }
    }

    return 0;
}

/*
 * Check file's cluster chain.
 *  - circular cluster chain / bad cluster
 *
 * Set cluster owner to 'file' entry to check circular chain.
 * After check reset owner to NULL.
 */
static void test_file(DOS_FS *fs, DOS_FILE *file, int read_test)
{
    DOS_FILE *owner;
    uint32_t walk, prev, clusters, next_clu;

    prev = clusters = 0;
    for (walk = FSTART(file, fs);
            walk > 0 && walk < fs->clusters + 2;
            walk = next_clu) {

        next_clu = next_cluster(fs, walk);
        if ((owner = get_owner(fs, walk))) {
            if (owner == file) {
                printf("%s\n  Circular cluster chain. "
                        "Truncating to %u cluster%s.\n",
                        path_name(file), clusters, clusters == 1 ? "" : "s");

                if (prev)
                    set_fat(fs, prev, -1);
                else if (!file->offset)
                    die("Bad FAT32 root directory! (bad start cluster)\n");
                else
                    MODIFY_START(file, 0, fs);
            }
            break;
        }

        if (bad_cluster(fs, walk))
            break;

        if (!read_test) {
            /* keep cluster walk */
            prev = walk;
            clusters++;
        }
        else { /* if (read_test) */
            if (fs_test(cluster_start(fs, walk), fs->cluster_size)) {
                prev = walk;
                clusters++;
            }
            else {
                printf("%s\n  Cluster %u (%u) is unreadable. Skipping it.\n",
                        path_name(file), clusters, walk);
                if (prev)
                    set_fat(fs, prev, next_cluster(fs, walk));
                else
                    MODIFY_START(file, next_cluster(fs, walk), fs);

                set_fat(fs, walk, -2);
            }
        }
        set_owner(fs, walk, file);
    }

    for (walk = FSTART(file, fs); walk > 0 && walk < fs->clusters + 2;
            walk = next_cluster(fs, walk)) {
        if (bad_cluster(fs, walk))
            break;
        else if (get_owner(fs, walk) == file)
            set_owner(fs, walk, NULL);
        else
            break;
    }
}

static void undelete(DOS_FS *fs, DOS_FILE *file)
{
    uint32_t clusters, left, prev, walk;

    clusters = left = (CF_LE_L(file->dir_ent.size) +
            fs->cluster_size - 1) / fs->cluster_size;
    prev = 0;
    for (walk = FSTART(file, fs); left && walk >= 2 && walk <
            fs->clusters + 2 && !fs->fat[walk].value; walk++) {
        left--;
        if (prev)
            set_fat(fs, prev, walk);
        prev = walk;
    }

    if (prev)
        set_fat(fs, prev, -1);
    else
        MODIFY_START(file, 0, fs);

    if (left)
        printf("Warning: Did only undelete %u of %u cluster%s.\n",
                clusters - left, clusters, clusters == 1 ? "" : "s");

}

static void new_dir(void)
{
    lfn_reset();
}

static void add_file(DOS_FS *fs, DOS_FILE ***chain, DOS_FILE *parent,
        loff_t offset, FDSC **cp)
{
    DOS_FILE *new;
    DIR_ENT de;
    FD_TYPE type;

    if (offset)
        fs_read(offset, sizeof(DIR_ENT), &de);
    else {
        memcpy(de.name, "           ", MSDOS_NAME);
        de.attr = ATTR_DIR;
        de.size = de.time = de.date = 0;
        de.start = CT_LE_W(fs->root_cluster & 0xffff);
        de.starthi = CT_LE_W((fs->root_cluster >> 16) & 0xffff);
    }

    /* check ".", ".." not in 1st/2nd entry */
    if (!strncmp((char *)de.name, MSDOS_DOT, LEN_FILE_NAME) ||
            !strncmp((char *)de.name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
        int dot = 0;

        if (!strncmp((char *)de.name, MSDOS_DOT, LEN_FILE_NAME)) {
            dot = 1;
        }
        printf("Found invalid %s entry on (%s%s%s)\n",
                dot ? "dot" : "dotdot", path_name(parent),
                parent->lfn ? "/" : "",
                dot ? "." : "..");

        if (interactive)
            printf("1: Delete.\n"
                    "2: Leave it.\n");
        else
            printf("  Auto-deleting.\n");

        if (!interactive || get_key("12", "?") == '1') {
            de.name[0] = DELETED_FLAG;
            fs_write(offset, sizeof(DIR_ENT), &de);
        }

        return;
    }

    if ((type = file_type(cp, (char *)de.name)) != fdt_none) {
        if (type == fdt_undelete && (de.attr & ATTR_DIR))
            die("Can't undelete directories.");
        file_modify(cp, (char *)de.name);
        fs_write(offset, 1, &de);
    }

    if (IS_FREE(de.name)) {
        lfn_check_orphaned();
        return;
    }

    if (de.attr == VFAT_LN_ATTR) {
        lfn_add_slot(&de, offset);
        return;
    }

    new = qalloc(&mem_queue, sizeof(DOS_FILE));
    new->lfn = lfn_get(&de);
    new->offset = offset;
    memcpy(&new->dir_ent, &de, sizeof(de));
    new->next = new->first = NULL;
    new->parent = parent;

    if (type == fdt_undelete)
        undelete(fs, new);

    **chain = new;
    *chain = &new->next;

    if (list) {
        printf("Checking file %s", path_name(new));
        if (new->lfn)
            printf(" (%s)", file_name(new->dir_ent.name));
        printf("\n");
    }

    if (offset &&
            strncmp((char *)de.name, MSDOS_DOT, MSDOS_NAME) != 0 &&
            strncmp((char *)de.name, MSDOS_DOTDOT, MSDOS_NAME) != 0)
        ++n_files;

    test_file(fs, new, test);
}

static int subdirs(DOS_FS *fs, DOS_FILE *parent, FDSC **cp);

static int scan_dir(DOS_FS *fs, DOS_FILE *this, FDSC **cp)
{
    DOS_FILE **chain;
    int offset;
    int ret = 0;
    uint32_t clu_num;

    chain = &this->first;
    offset = 0;
    clu_num = FSTART(this, fs);

    /* check here for first entry "." and second entry ".."
     * do not call add_file() for ".", ".." entries */

    /* do not check on root directory, because root directory does not have
     * dot and dotdot entry */
    if (this != root && clu_num > 0 && clu_num != -1) {
        /* check first entry */
        ret = check_dots(fs, this, DOT_ENTRY);
        if (ret)
            return -1;

        /* check second entry */
        ret = check_dots(fs, this, DOTDOT_ENTRY);
        if (ret)
            return -1;

        clu_num = FSTART(this, fs);
        offset = sizeof(DIR_ENT) * 2;   /* first, second entry skip */
    }

    new_dir();

    while (clu_num > 0 && clu_num != -1) {
        add_file(fs, &chain, this,
                cluster_start(fs, clu_num) + (offset % fs->cluster_size), cp);
        offset += sizeof(DIR_ENT);
        if (!(offset % fs->cluster_size))
            if ((clu_num = next_cluster(fs, clu_num)) == 0 || clu_num == -1)
                break;
    }

    lfn_check_orphaned();
    if (check_dir(fs, &this->first, this->offset))
        return 0;

    if (check_files(fs, this->first))
        return 1;

    return subdirs(fs, this, cp);
}

static int subdirs(DOS_FS *fs, DOS_FILE *parent, FDSC **cp)
{
    DOS_FILE *walk;

    for (walk = parent ? parent->first : root; walk; walk = walk->next)
        if (walk->dir_ent.attr & ATTR_DIR)
            if (scan_dir(fs, walk, file_cd(cp, (char *)(walk->dir_ent.name))))
                return 1;
    return 0;
}

int scan_root(DOS_FS *fs)
{
    DOS_FILE **chain;
    int i;

    root = NULL;
    chain = &root;
    new_dir();
    if (fs->root_cluster) {
        add_file(fs, &chain, NULL, 0, &fp_root);
    }
    else {
        for (i = 0; i < fs->root_entries; i++)
            add_file(fs, &chain, NULL,
                    fs->root_start + i * sizeof(DIR_ENT), &fp_root);
    }

    lfn_check_orphaned();
    (void)check_dir(fs, &root, 0);

    if (check_files(fs, root))
        return 1;

    return subdirs(fs, NULL, &fp_root);
}

void scan_root_only(DOS_FS *fs, label_t **head, label_t **last)
{
    DOS_FILE **chain;
    DOS_FILE *this;
    int i;
    int offset = 0;
    uint32_t clus_num;

    root = NULL;
    chain = &root;
    new_dir();
    if (fs->root_cluster) {
        add_file(fs, &chain, NULL, 0, &fp_root);
    }
    else {
        for (i = 0; i < fs->root_entries; i++)
            add_file(fs, &chain, NULL,
                    fs->root_start + i * sizeof(DIR_ENT), &fp_root);
    }

    chain = &root->first;
    this = root;
    clus_num = FSTART(this, fs);
    lfn_reset();

    while (clus_num > 0 && clus_num != -1) {
        add_file(fs, &chain, this,
                cluster_start(fs, clus_num) + (offset % fs->cluster_size),
                NULL);
        offset += sizeof(DIR_ENT);
        if (!(offset % fs->cluster_size))
            if ((clus_num = next_cluster(fs, clus_num)) == 0 || clus_num == -1)
                break;
    }

    scan_volume_entry(fs, head, last);
    lfn_reset();
}

/**
 * check if boot label is invalid.
 * - check label length
 * - check lower character
 * - check illegal character
 *
 * @return  return 0 if label is valid, or return -1
 */
int check_valid_label(char *label)
{
    int len = 0;
    int i;

    len = strlen(label);
    if (len > LEN_VOLUME_LABEL) {
        printf("labels can be no longer than 11 characters\n");
        return -1;
    }

    if (len <= 0) {
        return -1;
    }

    if (memcmp(label, LABEL_EMPTY, LEN_VOLUME_LABEL) == 0) {
        return -1;
    }

    if (memcmp(label, LABEL_NONAME, LEN_VOLUME_LABEL) == 0) {
        return 0;
    }

    /* check blank between characters */
    for (i = LEN_VOLUME_LABEL; label[i - 1] == 0x20 || !label[i - 1]; i--);
    len = i;

    for (i = 0; i < len - 1; i++) {
        if (label[i] == 0x20)
            return -1;
    }

    /* check bad character based on long file name specification */
    for (i = 0; i < len; i++) {
        if ((unsigned char)label[i] < 0x20) {
            printf("label has character less than 0x20\n");
            return -1;
        }
        else if (label[i] == 0x22 || label[i] == 0x2A ||
                label[i] == 0x2E || label[i] == 0x2F ||
                label[i] == 0x3A || label[i] == 0x3C ||
                label[i] == 0x3E || label[i] == 0x3F ||
                label[i] == 0x5C || label[i] == 0x7C) {
            printf("label has illegal character\n");
            return -1;
        }
    }

    return 0;
}

int check_boot_label(char *boot_label)
{
    char blabel[LEN_VOLUME_LABEL + 1] = {'\0', };

    memcpy(blabel, boot_label, strlen(boot_label));
    return check_valid_label(blabel);
}

int check_root_label(char *root_label)
{
    char rlabel[LEN_VOLUME_LABEL + 1] = {'\0', };

    memcpy(rlabel, root_label, LEN_VOLUME_LABEL);
    return check_valid_label(rlabel);
}

/**
 * get label from console to rename label
 *
 * @param[OUT] new_label
 */
void get_label(char *new_label)
{
    int ret = -1;
    int len;
    char *p;
    char temp_label[64] = {'\0', };

    do {
        printf("Input label: ");
        fflush(stdout);

        memset(temp_label, 0, 64);
        /* enough length to include unicode label length,
         * but it can not be longer than 11 characters.
         * check it's length after fgets() called */
        if (fgets(temp_label, 64, stdin)) {
            if ((p = strchr(temp_label, '\n')))
                *p = 0;

            len = strlen(temp_label);
            if (len > LEN_VOLUME_LABEL) {
                printf("Label can be no longer than 11 characters,"
                        " try again\n");
                continue;
            }

            ret = check_valid_label(temp_label);
            if (ret < 0) {
                printf("label is not valid\n");
                continue;
            }

            if (strlen(temp_label) > LEN_VOLUME_LABEL) {
                printf("volume label is larger than 11."
                        " truncate label to 11 length\n");
            }
            memcpy(new_label, temp_label, LEN_VOLUME_LABEL);
        }
    } while (ret < 0);
}

static void add_label_entry(DOS_FS *fs, DOS_FILE ***chain, DOS_FILE *parent,
        loff_t offset, DIR_ENT *de)
{
    DOS_FILE *new;

    new = qalloc(&mem_queue, sizeof(DOS_FILE));
    new->lfn = lfn_get(de);
    new->offset = offset;
    memcpy(&new->dir_ent, de, sizeof(*de));
    new->next = new->first = NULL;
    new->parent = parent;

    **chain = new;
    *chain = &new->next;
}

/**
 * add label in root entry with label_t structure.
 * in almost case, there is one root label entry.
 * ie. label head and tail has same pointer value.
 *
 * @param [IN]  fs  DOS_FS structure pointer
 * @param [IN]  label   root label to add
 * @param [IN/OUT]  head    root label's head pointer
 * @param [IN/OUT]  tail    root label's tail pointer
 */
void add_label(DOS_FILE *label, label_t **head, label_t **last)
{
    label_t *new = NULL;

    new = alloc(sizeof(label_t));
    new->file = label;
    new->next = NULL;
    new->flag = LABEL_FLAG_NONE;

    if ((*last)) {
        (*last)->next = new;
    }
    else {
        *head = new;
    }

    *last = new;

    /* check bad volume label in root entry */
    if (check_root_label((char *)label->dir_ent.name)) {
        new->flag = LABEL_FLAG_BAD;
    }
}

/**
 */
void del_label(label_t *label, label_t **prev, label_t **head, label_t **last)
{
    if (prev)
        (*prev)->next = label->next;

    if ((*head) == label)
        *head = label->next;

    if ((*last) == label) {
        if (prev) {
            *last = *prev;
        } else {
            *last = NULL;
        }
    }
    free(label);
}

void clean_label(label_t **head, label_t **last)
{
    label_t *this;

    while (*head) {
        this = (*head);
        (*head) = (*head)->next;

        if (this == (*last))
            (*last) = NULL;

        free(this);
    }
}

void write_root_label(DOS_FS *fs, char *label, label_t **head, label_t **last)
{
    DIR_ENT de;
    off_t offset;
    struct tm *ctime;
    time_t current;

    time(&current);
    ctime = localtime(&current);

    if (memcmp(label, LABEL_NONAME, LEN_VOLUME_LABEL) == 0) {
        /* do not need to set root label entry */
        return;
    }

    /* there's no volume label in root entry */
    if (!(*head)) {
        DOS_FILE **chain = NULL;
        DOS_FILE *walk = NULL;
        DOS_FILE *prev = NULL;

        /* add_new_label() */

        offset = alloc_rootdir_entry(fs, &de, NULL);
        memcpy(de.name, label, LEN_VOLUME_LABEL);
        chain = &root->first;

        /* find last chain of root entry */
        for (walk = root->first; walk; walk = walk->next) {
            chain = &walk->next;
            prev = walk;
        }
        add_label_entry(fs, &chain, root, offset, &de);
        add_label(prev->next, head, last);

        /**/
    }
    else {
        DOS_FILE *walk = (*head)->file;

        offset = walk->offset;
        memcpy(&de, &walk->dir_ent, sizeof(de));
        (*head)->flag = LABEL_FLAG_NONE;
    }

    memcpy(de.name, label, LEN_VOLUME_LABEL);
    /* for KANJI lead byte of japanese,
     * TODO: check other place to apply this */
    if (de.name[0] == 0xe5)
        de.name[0] = 0x05;

    de.attr = ATTR_VOLUME;
    de.time = CT_LE_W((unsigned short)((ctime->tm_sec >> 1) +
                (ctime->tm_min << 5) + (ctime->tm_hour << 11)));
    de.date = CT_LE_W((unsigned short)(ctime->tm_mday +
                ((ctime->tm_mon + 1) << 5) +
                ((ctime->tm_year - 80) << 9)));
    de.ctime_ms = 0;
    de.ctime = de.time;
    de.cdate = de.date;
    de.adate = de.date;
    de.starthi = 0;
    de.start = 0;
    de.size = 0;

    fs_write(offset, sizeof(DIR_ENT), &de);
}

void write_boot_label(DOS_FS *fs, char *label)
{
    struct boot_sector b;
    struct volume_info *vi = NULL;
    char *fs_type = NULL;

    fs_read(0, sizeof(b), &b);

    if (fs->fat_bits == 12 || fs->fat_bits == 16) {
        vi = &b.oldfat.vi;
        fs_type = fs->fat_bits == 12 ? MSDOS_FAT12_SIGN: MSDOS_FAT16_SIGN;
    }
    else if (fs->fat_bits == 32) {
        vi = &b.fat32.vi;
        fs_type = MSDOS_FAT32_SIGN;
    }
    else
        die("Can't find fat fs type");

    if (vi->extended_sig != MSDOS_EXT_SIGN) {
        vi->extended_sig = MSDOS_EXT_SIGN;
        memset(vi->volume_id, 0, 4);
        memcpy(vi->fs_type, fs_type, 8);
    }
    memcpy(vi->label, label, LEN_VOLUME_LABEL);
    fs_write(0, sizeof(b), &b);

    if (fs->backupboot_start)
        fs_write(fs->backupboot_start, sizeof(b), &b);

}

/**
 * write boot and root label.
 * if this function is called from remove_label,
 * (parameter 'label' value is LABEL_NONAME)
 * set root label in cast just only it already exist.
 * (do not allocate root label entry to set LABEL_NONAME)
 *
 * @param[IN]   label   label string to set root/boot
 */
void write_label(DOS_FS *fs, char *label, label_t **head, label_t **last)
{
    int len = 0;

    if (!label)
        return;

    len = strlen(label);
    while (len < LEN_VOLUME_LABEL)
        label[len++] = ' ';

    write_boot_label(fs, label);
    write_root_label(fs, label, head, last);

    memcpy(fs->label, label, LEN_VOLUME_LABEL);
}


static void remove_boot_label(DOS_FS *fs)
{
    write_boot_label(fs, LABEL_NONAME);
    memcpy(fs->label, LABEL_NONAME, LEN_VOLUME_LABEL);
}

/**
 * @param[IN]   label   root label entry pointer
 */
static void remove_root_label(DOS_FILE *label)
{
    if (label) {
        label->dir_ent.name[0] = DELETED_FLAG;
        label->dir_ent.attr = 0;
        fs_write(label->offset, sizeof(DIR_ENT), &label->dir_ent);
    }
}

/**
 * remove boot and root label.
 * if root label exist, set DELETED_FLAG to it.
 * and set "NO NAME    " to boot label
 *
 * @param [IN] label    root label entry pointer to remove
 */
void remove_label(DOS_FS *fs, DOS_FILE *label, label_t **head, label_t **last)
{
    remove_root_label(label);
    write_label(fs, LABEL_NONAME, head, last);
}

/**
 * scan all of root entries and find root volume entries.
 * make data structure for root label entries.
 * this function should be called after scan_root()
 *
 * @param [IN]  fs  DOS_FS structure pointer
 * @param [IN/OUT]  head    root label's head pointer
 * @param [IN/OUT]  tail    root label's tail pointer
 * */
void scan_volume_entry(DOS_FS *fs, label_t **head, label_t **last)
{
    DOS_FILE *walk = NULL;

    if (*head && *last) {
        printf("Already scanned volume label entries\n");
        return;
    }

    for (walk = fs->root_cluster ? root->first : root;
            walk; walk = walk->next) {
        if (IS_FREE(walk->dir_ent.name) ||
                IS_LFN_ENT(walk->dir_ent.attr) ||
                !IS_VOLUME_LABEL(walk->dir_ent.attr)) {
            continue;
        }

        /* allocate label_t and set */
        add_label(walk, head, last);
    }
}

/**
 * check lists for volume label
 * - if there are volume label more than 2 entries in root, remove except one.
 * - if root label is not valid, remove it.
 *   . empty label / label has non-permitted character / label is lower
 * - after removal of volume label, volume label set to "NO NAME    ".
 * - volume label should have zero length and zero start cluster number
 * - use label in root entry (copy root label to boot label),
 *   if there are different volume label in boot sector and root entry.
 *
 * @return  return 0 success, or -1 if an error occurred
 */
int check_volume_label(DOS_FS *fs)
{
    int ret = 0;
    char label_temp[LEN_VOLUME_LABEL + 1] = {'\0', };
    DOS_FILE *walk = NULL;  /* DOS_FILE walk */
    label_t *lwalk = NULL;  /* label_t walk */
    label_t **prev = NULL;

    /* find root volume entries and make label_t structure */
    scan_volume_entry(fs, &label_head, &label_last);

    /* in case that there is no root volume label */
    if (!label_head) {
        if (memcmp(fs->label, LABEL_NONAME, LEN_VOLUME_LABEL) == 0) {
            /* normal case,
             * TODO: initialize volume label to LABEL_NONAME in mkdosfs */
            ret = 0;
            goto exit;
        }
        /* check bad volume label in boot sector */
        if (check_boot_label(fs->label) == -1) {
            printf("Volume label '%s' in boot sector is not valid.\n",
                    fs->label);
            if (interactive)
                printf("1) Remove invalid boot label\n"
                        "2) Set new label\n");
            else
                printf("  Auto-removing label from boot sector.\n");

            switch (interactive ? get_key("12", "?") : '1') {

                case '1':
                    remove_label(fs, NULL, &label_head, &label_last);
                    break;
                case '2': {
                    char new_label[LEN_VOLUME_LABEL + 1] = {'\0', };

                    get_label(new_label);
                    write_label(fs, new_label, &label_head, &label_last);
                    break;
                }
            }
        }
        else {
            printf("Label in boot is '%s', "
                    "but there is no label in root directory.\n",
                    fs->label);

            if (interactive)
                printf("1) Remove root label\n"
                        "2) Copy boot label to root label entry\n");
            else
                printf("  Auto-removing label from boot sector.\n");

            switch (interactive ? get_key("12", "?") : '1') {
                case '1':
                    remove_label(fs, NULL, &label_head, &label_last);
                    break;
                case '2':
                    /* write root label */
                    write_root_label(fs, fs->label, &label_head, &label_last);
            }
        }

        ret = 0;
        goto exit;
    }

    walk = NULL;
    /* handle multiple root volume label */
    if (label_head && label_head != label_last) {
        int idx = 0;
        int choose = 0;

        printf("Multiple volume label in root\n");
        for (lwalk = label_head; lwalk; lwalk = lwalk->next) {
            walk = lwalk->file;
            memcpy(label_temp, walk->dir_ent.name, LEN_VOLUME_LABEL);
            printf("  %d - %s\n", idx + 1, label_temp);
            idx++;
        }

        if (interactive)
            printf("1) Remove all label\n"
                    "2) Auto Select one label(first)\n"
                    "3) Select one label to leave\n");
        else
            printf("  Auto-removing label%s in root entry except one\n",
                    idx > 1 ? "s" : "");

        switch (interactive ? get_key("123", "?") : '2') {
            case '1':
                prev = NULL;
                for (lwalk = label_head; lwalk;) {
                    remove_root_label(lwalk->file);
                    /* lwalk/label_head/label_last might change in del_label */
                    del_label(lwalk, prev, &label_head, &label_last);
                    lwalk = label_head;
                }

                remove_boot_label(fs);
                goto exit;

            case '2':
                walk = label_head->file;
                memcpy(label_temp, walk->dir_ent.name, LEN_VOLUME_LABEL);
                printf("  Select first label (%s)\n", label_temp);

                prev = &label_head;
                for (lwalk = label_head->next; lwalk;) {
                    remove_root_label(lwalk->file);
                    /* lwalk/label_head/label_last might change in del_label */
                    del_label(lwalk, prev, &label_head, &label_last);
                    lwalk = label_head->next;
                }

                write_boot_label(fs, label_temp);
                /* need to check valid label : LABEL_FLAG_BAD */
                break;
            case '3':
                do {
                    /* FIXME: max 9 label entries can be selected */
                    choose = get_key("123456789", "  Select label number : ");
                    choose -= '0';
                    if (choose > idx)
                        printf("  Invalid label index(%d)."
                                " Select again.(1~%d)\n", choose, idx);
                } while (choose > idx);

                prev = NULL;
                for (lwalk = label_head, idx = 1; lwalk; idx++) {
                    /* do not remove selected label */
                    if (choose == idx) {
                        walk = lwalk->file;
                        prev = &label_head;
                        lwalk = lwalk->next;
                        continue;
                    }

                    remove_root_label(lwalk->file);
                    /* lwalk freed in del_label */
                    del_label(lwalk, prev, &label_head, &label_last);

                    if (prev)
                        lwalk = (*prev)->next;
                    else
                        lwalk = label_head;
                }

                memcpy(label_temp, walk->dir_ent.name, LEN_VOLUME_LABEL);
                printf("  Selected label (%s)\n", label_temp);

                write_boot_label(fs, label_temp);
                /* need to check valid label : LABEL_FLAG_BAD */
                break;
        }
    }

    if (label_head != label_last) {
        printf("Error!!! There are still more than one root label entries\n");
        ret = -1;
        goto exit;
    }

    if (!label_head) {
        printf("Error!! There is still no root label\n");
        ret = -1;
        goto exit;
    }

    lwalk = label_head;
    walk = lwalk->file;

    memcpy(label_temp, walk->dir_ent.name, LEN_VOLUME_LABEL);

    /* handle bad label in root entry */
    if (lwalk->flag & LABEL_FLAG_BAD) {
        printf("Label '%s' in root entry is not valid\n", label_temp);
        if (interactive)
            printf("1) Remove invalid root label\n"
                    "2) Set new label\n");
        else
            printf("  Auto-removing label in root entry.\n");

        switch (interactive ? get_key("12", "?") : '1') {
            case '1':
                remove_label(fs, lwalk->file, &label_head, &label_last);
                break;
            case '2': {
                char new_label[LEN_VOLUME_LABEL + 1] = {'\0', };

                get_label(new_label);
                write_label(fs, new_label, &label_head, &label_last);
                break;
            }
        }
        ret = 0;
        goto exit;
    }

    /* check boot label, boot label is valid here */
    if (check_valid_label(fs->label) == -1) {
        printf("Label '%s' in boot sector is not valid."
                " but label '%s' in root entry is valid.\n",
                fs->label, label_temp);
        if (interactive)
            printf("1) Copy label from root entry to boot\n"
                    "2) Set new label\n");
        else
            printf("  Auto-copying label from root entry.\n");

        switch (interactive ? get_key("12", "?") : '1') {
            case '1':
                write_boot_label(fs, label_temp);
                memcpy(fs->label, label_temp, LEN_VOLUME_LABEL);
                break;
            case '2': {
                char new_label[LEN_VOLUME_LABEL + 1] = {'\0', };

                get_label(new_label);
                write_label(fs, new_label, &label_head, &label_last);
                break;
            }
        }
        ret = 0;
        goto exit;
    }

    /* handle different volume label in boot and root */
    if (memcmp(fs->label, label_temp, LEN_VOLUME_LABEL) != 0) {

        printf("Label '%s' in root entry "
                "and label '%s' in boot sector are different\n",
                label_temp, fs->label);
        if (memcmp(fs->label, LABEL_NONAME, LEN_VOLUME_LABEL) == 0) {
            printf("Copy label from root entry(%s)\n", label_temp);
            write_label(fs, label_temp, &label_head, &label_last);
            ret = 0;
            goto exit;
        }

        if (interactive) {
            printf("1) Copy label from boot to root entry\n"
                    "2) Copy label from root entry to boot\n");
        }
        else {
            printf("  Auto-copying label from root entry to boot\n");
        }

        switch (interactive ? get_key("12", "?") : '2') {
            case '1':
                write_root_label(fs, fs->label, &label_head, &label_last);
                break;
            case '2':
                write_boot_label(fs, label_temp);
                memcpy(fs->label, label_temp, LEN_VOLUME_LABEL);
                break;
        }
    }

exit:
    clean_label(&label_head, &label_last);
    return ret;
}

/* allocate new cluster and add dot / dotdot entry */
static int add_dot_entries(DOS_FS *fs, DOS_FILE *parent, int dots)
{
    loff_t offset = 0;
    loff_t start_offset;
    loff_t new_offset;

    uint32_t start_clus;    /* start cluster number of parent */
    uint32_t new_clus;      /* new allocated cluster number */
    uint32_t next_clus;     /* original next cluster chain of start_cluster */

    DOS_FILE dot_file;
    DOS_FILE *file;
    DIR_ENT *de;
    DIR_ENT *p_de;
    char *entry_name;

    int i;
    int ent_size;

    file = &dot_file;
    de = &dot_file.dir_ent;
    p_de = &parent->dir_ent;

    /* find free cluster */
    for (new_clus = FAT_START_ENT + 1; new_clus != FAT_START_ENT; new_clus++) {
        if (new_clus >= fs->clusters + FAT_START_ENT)
            new_clus = FAT_START_ENT;
        if (!fs->fat[new_clus].value)
            break;
    }

    if (new_clus == FAT_START_ENT) {
        die("Can't find free cluster\n");
    }

    /* allocate new cluster and make it second cluster,
     * move entries of first cluster to new cluster,
     * and add dot / dotdot entry on first cluster */

    ent_size = sizeof(DIR_ENT);

    start_clus = FSTART(parent, fs);
    new_offset = cluster_start(fs, new_clus);
    start_offset = cluster_start(fs, start_clus);

    /* check for 1st, 2nd entry of start_cluster */
    for (i = 0; i < (2 * ent_size); i += ent_size) {
        fs_read(start_offset + i, ent_size, de);
        if (strncmp((char *)de->name, MSDOS_DOT, LEN_FILE_NAME) &&
                strncmp((char *)de->name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
            fs_write(new_offset + i, ent_size, de);
            de->name[0] = DELETED_FLAG;
            fs_write(start_offset + i, ent_size, de);
        }
        else {
            memset(de, 0, sizeof(DIR_ENT));
            fs_write(new_offset + i, ent_size, de);
        }
    }

    /* start on 3rd entry */
    for (i = (2 * ent_size); i < fs->cluster_size; i += ent_size) {
        /* copy entries of start_cluster to new_cluster
         * except ".", ".." entries */
        fs_read(start_offset + i, ent_size, de);
        fs_write(new_offset + i, ent_size, de);
        de->name[0] = DELETED_FLAG;
        fs_write(start_offset + i, ent_size, de);
    }

    next_clus = fs->fat[start_clus].value;
    set_fat(fs, start_clus, new_clus);
    set_fat(fs, new_clus, next_clus);
    set_owner(fs, new_clus, get_owner(fs, start_clus));

    if (dots == DOT_ENTRY) {
        /* first entry offset */
        offset = 0;
        entry_name = MSDOS_DOT;
    }
    else {
        /* second entry offset */
        offset = ent_size;
        entry_name = MSDOS_DOTDOT;
    }

    /* make and write dot / dotdot DE entries */
    memset(de, 0, ent_size);
    memcpy(de->name, entry_name, LEN_FILE_NAME);
    de->attr = ATTR_DIR;
    de->ctime_ms = p_de->ctime_ms;
    de->ctime = p_de->ctime;
    de->cdate = p_de->cdate;
    de->adate = p_de->adate;
    de->time = p_de->time;
    de->date = p_de->date;

    file->offset = start_offset + offset;
    fs_write(file->offset, ent_size, de);

    if (dots == DOT_ENTRY) {
        MODIFY_START(file, start_clus, fs);
    }
    else {
        /* dots == DOTDOT_ENTRY */
        if (parent->parent == root) {
            MODIFY_START(file, 0, fs);
        }
        else {
            MODIFY_START(file, FSTART(parent->parent, fs), fs);
        }
    }

    return 0;
}

static int check_dots(DOS_FS *fs, DOS_FILE *parent, int dots)
{
    uint32_t start_clus;
    uint32_t clus_num;
    loff_t offset;
    char *entry_name;
    DOS_FILE file;
    DOS_FILE *dot_file;
    DIR_ENT *p_de;
    DIR_ENT *de;

    dot_file = &file;

    if (parent == root) {
        /* 'parent' is root directory's entry,
         * root directory does not have ".", ".." entries. */
        die("%s can't be called on root directory.", __func__);
        return -1;
    }

    clus_num = FSTART(parent, fs);
    if (dots == DOT_ENTRY) {
        entry_name = MSDOS_DOT;
        start_clus = clus_num;
        offset = 0;
    }
    else {
        entry_name = MSDOS_DOTDOT;
        start_clus = FSTART(parent->parent, fs);
        offset = sizeof(DIR_ENT);
    }

    memset(dot_file, 0, sizeof(DOS_FILE));
    dot_file->parent = parent;
    dot_file->offset = cluster_start(fs, clus_num) + offset;
    fs_read(dot_file->offset, sizeof(DIR_ENT), &dot_file->dir_ent);

    if (dots == DOTDOT_ENTRY && parent->parent == root) {
        /* parent of 'parent' is root directory */
        if (start_clus != fs->root_cluster) {
            die("root_cluster is different with root start cluster\n");
        }
        start_clus = 0;
    }

    p_de = &parent->dir_ent;
    de = &dot_file->dir_ent;
    if (strncmp((char *)de->name, entry_name, LEN_FILE_NAME) == 0) {
        if (list)
            printf("Checking file %s\n", path_name(dot_file));

        if (!(de->attr & ATTR_DIR)) {
            printf("%s\n  Fixing %s entry's('%s') invalid attribute.\n",
                    path_name(dot_file->parent),
                    (dots == DOT_ENTRY) ? "first" : "second",
                    (dots == DOT_ENTRY) ? "." : "..");
            MODIFY(dot_file, attr, ATTR_DIR);
        }

        if (start_clus != FSTART(dot_file, fs)) {
            printf("%s\n  Fixing %s entry's('%s') invalid start cluster.\n",
                    path_name(dot_file->parent),
                    (dots == DOT_ENTRY) ? "first" : "second",
                    (dots == DOT_ENTRY) ? "." : "..");
            MODIFY_START(dot_file, start_clus, fs);
        }

        if (memcmp(&p_de->ctime_ms, &de->ctime_ms, 7) ||
                    memcmp(&p_de->time, &de->time, 4)) {
            /* copy ctime_ms, ctime, cdate, adate, time, date from self */
            MODIFY(dot_file, ctime_ms, p_de->ctime_ms);
            MODIFY(dot_file, ctime, p_de->ctime);
            MODIFY(dot_file, cdate, p_de->cdate);
            MODIFY(dot_file, adate, p_de->adate);
            MODIFY(dot_file, time, p_de->time);
            MODIFY(dot_file, date, p_de->date);
        }
        return 0;
    }

    /* dot/dotdot entry is not exist */
    /* 1. deleted case
     * 2. other entry is located in first/second entry */
    if (IS_FREE(de->name)) {
        printf("%s\n  %s entry is expected as '%s',"
                " but is free or deleted.\n",
                path_name(dot_file->parent),
                (dots == DOT_ENTRY) ? "first" : "second",
                (dots == DOT_ENTRY) ? "." : "..");
        if (interactive)
            printf("1) Create %s entry\n"
                    "2) Drop parent entry\n",
                    (dots == DOT_ENTRY) ? "first" : "second");
        else
            printf("  Auto-creating entry.\n");

        switch (interactive ? get_key("12", "?") : '1') {
            case '1':
            {
                /* check second entry status before setting "." in first entry
                 * if ".." can't be set in second entry
                 * because of other valid entry,
                 * call add_dot_entries() to allocate new cluster and
                 * set both ".", ".." entries */
                if (dots == DOT_ENTRY) {
                    DIR_ENT next_de;

                    /* read next(second) entry */
                    fs_read(offset + sizeof(DIR_ENT), sizeof(DIR_ENT), &next_de);

                    if (strncmp((char *)de->name, MSDOS_DOTDOT, LEN_FILE_NAME) &&
                            !IS_FREE(next_de.name)) {

                        add_dot_entries(fs, parent, dots);
                        return 0;
                    }
                }

                memcpy(de->name, entry_name, LEN_FILE_NAME);
                de->attr = ATTR_DIR;
                de->ctime_ms = p_de->ctime_ms;
                de->ctime = p_de->ctime;
                de->cdate = p_de->cdate;
                de->adate = p_de->adate;
                de->time = p_de->time;
                de->date = p_de->date;
                fs_write(dot_file->offset, sizeof(dot_file->dir_ent), de);
                MODIFY_START(dot_file, start_clus, fs);
                break;
            }
            case '2':
                drop_file(fs, parent);
                return 1;
        }
        return 0;
    }

    /* first/second entry is not ".", "..", and valid LFN/DE.
     * allocate new cluster for first/second entry,
     * write ".", ".." entry to allocated new cluster. */

    printf("%s\n  %s entry is expected as '%s', but is '%s'.\n",
            path_name(dot_file),
            (dots == DOT_ENTRY) ? "First" : "Second",
            (dots == DOT_ENTRY) ? "." : "..",
            IS_LFN_ENT(de->attr) ? "LFN entry" : file_name(de->name));

    if (interactive) {
        printf("1) Drop '%s' entry\n"
                "2) Drop parent entry\n"
                "3) Add dots entries to first/second\n",
                file_name(de->name));
    }
    else
        printf("  Auto-adding dots entries to first/second.\n");

    switch (interactive ? get_key("123", "?") : '3') {
        case '1':
            offset = dot_file->offset;
            drop_file(fs, dot_file);
            dot_file->offset = offset;

            memcpy(de->name, entry_name, LEN_FILE_NAME);
            de->attr = ATTR_DIR;
            de->ctime_ms = p_de->ctime_ms;
            de->ctime = p_de->ctime;
            de->cdate = p_de->cdate;
            de->adate = p_de->adate;
            de->time = p_de->time;
            de->date = p_de->date;
            fs_write(dot_file->offset, sizeof(DIR_ENT), de);
            MODIFY_START(dot_file, start_clus, fs);
            break;
        case '2':
            drop_file(fs, parent);
            return 1;
        case '3':
            add_dot_entries(fs, parent, dots);
            break;
    }
    return 0;
}

int check_dirty_flag(DOS_FS *fs)
{
    FAT_ENTRY fat_2nd;
    uint32_t dirty_mask;

    if (fs->fat_bits == 32) {
        dirty_mask = FAT32_DIRTY_BIT_MASK;
    }
    else {
        dirty_mask = FAT16_DIRTY_BIT_MASK;
    }

    /* read second value of FAT that has dirty flag */
    get_fat(fs, 1, &fat_2nd);
    if ((fs->fat_state & FAT_STATE_DIRTY) || !(fat_2nd.value & dirty_mask)) {
        printf("FAT dirty flag is set.\n"
                "  Filesystem might be shudowned unexpectedly,\n"
                "  So filesystem may be corrupted.\n\n");
        return -1;
    }
    printf("FAT dirty flag is clean.\n");
    return 0;
}

void clean_dirty_flag(DOS_FS *fs)
{
    FAT_ENTRY fat_2nd;
    uint32_t dirty_mask;
    struct boot_sector b;
    struct volume_info *vi = NULL;

    fs_read(0, sizeof(b), &b);

    if (fs->fat_bits == 32) {
        dirty_mask = FAT32_DIRTY_BIT_MASK;
        vi = &b.fat32.vi;
    }
    else {
        dirty_mask = FAT16_DIRTY_BIT_MASK;
        vi = &b.oldfat.vi;
    }

    get_fat(fs, 1, &fat_2nd);

    if ((fs->fat_state & FAT_STATE_DIRTY) || !(fat_2nd.value & dirty_mask)) {
        if (interactive) {
            printf("1) Clean dity flag\n"
                    "2) Leave it\n");
        }
        else
            printf("  Auto-cleaning dirty flag\n");

        switch (interactive ? get_key("12", "?") : '1') {
            case '1':
                if (fs->fat_state & FAT_STATE_DIRTY) {
                    vi->state &= ~FAT_STATE_DIRTY;
                    fs_write(0, sizeof(b), &b);
                }

                if (!(fat_2nd.value & dirty_mask)) {
                    set_fat(fs, 1, fat_2nd.value | dirty_mask);
                }

                fs->fat_state &= ~FAT_STATE_DIRTY;
                break;
            case '2':
                break;
        }
    }
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */