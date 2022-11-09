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

static DOS_FILE *root;

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
            fs_write((loff_t)offsetof(struct boot_sector, root_cluster),	\
                    sizeof(((struct boot_sector *)0)->root_cluster),	\
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
        DIR_ENT *root;
        int next_free = 0, scan;

        root = alloc(fs->root_entries * sizeof(DIR_ENT));
        fs_read(fs->root_start, fs->root_entries * sizeof(DIR_ENT), root);

        while (next_free < fs->root_entries) {
            if (IS_FREE(root[next_free].name) &&
                    root[next_free].attr != VFAT_LN_ATTR)
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
                        !strncmp((char *)root[scan].name,
                            (char *)de->name, MSDOS_NAME))
                    break;
            if (scan == fs->root_entries)
                break;

            if (++curr_num >= 10000)
                die("Unable to create unique name");
        }
        free(root);
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

    remove_lfn(fs, file);
    MODIFY(file, name[0], DELETED_FLAG);
    for (cluster = FSTART(file, fs);
            cluster > 0 && cluster < fs->clusters + 2;
            cluster = next_cluster(fs, cluster)) {
        set_owner(fs, cluster, NULL);
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

    if (de.attr == VFAT_LN_ATTR) {
        scan_lfn(&de, offset);
        return 0;
    }

    if (memcmp(de.name, file->dir_ent.name, 11) == 0 &&
            offset == file->offset) {
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
    loff_t offset = 0;

    clus_num = FSTART(parent, fs);
    clus_size = fs->cluster_size;

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

    parent = file->parent;
    if (!parent) {
        printf("Can't remove lfn of root entry\n");
        return;
    }

    if (find_lfn(fs, parent, file)) {
        lfn_remove();
    }
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

/* call only in case that directory has entries ".", ".." in check_dir().
 * if it is root directory, called with zero value of dots. */
static int handle_dot(DOS_FS *fs, DOS_FILE *file, int dots)
{
    char *name;

    name = strncmp((char *)file->dir_ent.name, MSDOS_DOT, MSDOS_NAME) ? ".." : ".";
    if (!(file->dir_ent.attr & ATTR_DIR)) {
        printf("%s\n  Is a non-directory.\n", path_name(file));
        if (interactive)
            printf("1) Drop it\n"
                    "2) Auto-rename\n"
                    "3) Rename\n"
                    "4) Convert to directory\n");
        else
            printf("  Auto-renaming it.\n");

        switch (interactive ? get_key("1234", "?") : '2') {
            case '1':
                drop_file(fs, file);
                return 1;
            case '2':
                auto_rename(fs, file);
                printf("  Renamed to %s\n", file_name(file->dir_ent.name));
                return 0;
            case '3':
                rename_file(fs, file);
                return 0;
            case '4':
                MODIFY(file, size, CT_LE_L(0));
                MODIFY(file, attr, file->dir_ent.attr | ATTR_DIR);
                break;
        }
    }

    if (!dots) {
        printf("Root contains directory \"%s\". Dropping it.\n", name);
        drop_file(fs, file);
        return 1;
    }
    return 0;
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
    int dot, dotdot, skip, redo;
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

    dot = dotdot = redo = 0;
    walk = root;
    while (*walk) {
        /* It check ".", ".." entries,
         * but now DOT/DOTDOT entry does not included in walk path
         * becuase subdirs() exclude ".", ".." entries, and check_dir() called
         * only through subdir() */
        if (!strncmp((char *)((*walk)->dir_ent.name), MSDOS_DOT, MSDOS_NAME) ||
                !strncmp((char *)((*walk)->dir_ent.name), MSDOS_DOTDOT, MSDOS_NAME)) {
            if (handle_dot(fs, *walk, dots)) {
                *walk = (*walk)->next;
                continue;
            }
            if (!strncmp((char *)((*walk)->dir_ent.name), MSDOS_DOT, MSDOS_NAME))
                dot++;
            else
                dotdot++;
        }

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
            dot = dotdot = redo = 0;
        }
    }

    if (dots && !dot)
        printf("%s\n  \".\" is missing. Can't fix this yet.\n",
                path_name(parent));
    if (dots && !dotdot)
        printf("%s\n  \"..\" is missing. Can't fix this yet.\n",
                path_name(parent));
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
    int i;
    uint32_t clu_num;

    chain = &this->first;
    i = 0;
    clu_num = FSTART(this, fs);
    new_dir();

    while (clu_num > 0 && clu_num != -1) {
        add_file(fs, &chain, this,
                cluster_start(fs, clu_num) + (i % fs->cluster_size), cp);
        i += sizeof(DIR_ENT);
        if (!(i % fs->cluster_size))
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
            /* TODO: where is the routine that handle DOT / DOTDOT? */
            if (strncmp((char *)(walk->dir_ent.name), MSDOS_DOT, MSDOS_NAME) &&
                    strncmp((char *)(walk->dir_ent.name), MSDOS_DOTDOT, MSDOS_NAME))
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

            ret = check_boot_label(temp_label);
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
    struct boot_sector_16 b16;

    if (fs->fat_bits == 12 || fs->fat_bits == 16) {
        fs_read(0, sizeof(b16), &b16);
        if (b16.extended_sig != 0x29) {
            b16.extended_sig = 0x29;
            b16.serial = 0;
            memcpy(b16.fs_type,
                    fs->fat_bits == 12 ? "FAT12   " : "FAT16   ", 8);
        }
        memcpy(b16.label, label, LEN_VOLUME_LABEL);
        fs_write(0, sizeof(b16), &b16);
    }
    else if (fs->fat_bits == 32) {
        fs_read(0, sizeof(b), &b);
        if (b.extended_sig != 0x29) {
            b.extended_sig = 0x29;
            b.serial = 0;
            memcpy(b.fs_type, "FAT32   ", 8);
        }
        memcpy(b.label, label, LEN_VOLUME_LABEL);
        fs_write(0, sizeof(b), &b);

        if (fs->backupboot_start)
            fs_write(fs->backupboot_start, sizeof(b), &b);
    }
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

    for (walk = root->first; walk; walk = walk->next) {
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
                break;
        }
    }

    if (label_head != label_last) {
        printf("error!!! There are still more than one root label entries\n");
        ret = -1;
        goto exit;
    }

    lwalk = label_head;
    walk = lwalk->file;

    memcpy(label_temp, walk->dir_ent.name, LEN_VOLUME_LABEL);

    /* handle bad label in root entry */
    if (lwalk && (lwalk->flag & LABEL_FLAG_BAD)) {
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

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
