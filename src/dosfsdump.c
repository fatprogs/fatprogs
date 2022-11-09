/* SPDX-License-Identifier : GPL-2.0 */

/* dosfsdump.c  -  User interface */

#include "../version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "common.h"
#include "dosfs.h"
#include "io.h"
#include "fat.h"
#include "boot.h"

#define DUMP_FILENAME   "./dump.file"

#define DE_START_CLUSTER(fs, de) \
    ((uint32_t)CF_LE_W(de->start) | \
     (fs->fat_bits == 32 ? CF_LE_W(de->starthi) << 16 : 0))

typedef enum dflag {
    DUMP_RESERVED,  /* Dump reserved sectors only */
    DUMP_FAT,       /* Dump reserved sectors and FATs only */
    DUMP_ALL,       /* Dump ALL */
} dflag_t;

int fd_in;
int fd_out;
int verbose = 0;
int fat_num = 0;
int atari_format = 0;
dflag_t dump_flag = DUMP_ALL;
unsigned short reserved_cnt;
unsigned short sector_size;
unsigned short sec_per_fat;

char *buf_clus = NULL;
char *buf_sec = NULL;

static void traverse_tree(DOS_FS *fs, uint32_t clus_num, int attr);

static inline loff_t CLUSTER_OFFSET(DOS_FS *fs, uint32_t clus_num)
{
    return fs->data_start +
        ((loff_t)clus_num - FAT_START_ENT) * (unsigned long long)fs->cluster_size;
}

static inline uint32_t NEXT_CLUSTER(DOS_FS *fs, uint32_t clus_num)
{
    uint32_t value;

    value = fs->fat[clus_num].value;
    if (FAT_IS_BAD(fs, value))
        return -1;
    else
        return FAT_IS_EOF(fs, value) ? -1 : value;
}

static inline void GET_FAT_ENTRY(FAT_ENTRY *entry,
        void *fat, uint32_t cluster, DOS_FS *fs)
{
    unsigned char *ptr;

    switch (fs->fat_bits) {
        case 12:
            ptr = &((unsigned char *) fat)[cluster * 3 / 2];
            entry->value = 0xfff & (cluster & 1 ? (ptr[0] >> 4) | (ptr[1] << 4) :
                    (ptr[0] | ptr[1] << 8));
            break;
        case 16:
            entry->value = CF_LE_W(((unsigned short *)fat)[cluster]);
            break;
        case 32:
            /* According to M$, the high 4 bits of a FAT32 entry are reserved and
             * are not part of the cluster number. So we cut them off. */
            {
                uint32_t e = CF_LE_L(((unsigned int *)fat)[cluster]);
                entry->value = e & 0xfffffff;
                entry->reserved = e >> 28;
            }
            break;
        default:
            die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }
}

void dump_area(loff_t pos, int size, void *data)
{
    int ret = 0;

    if (llseek(fd_in, pos, SEEK_SET) != pos)
        pdie("Seek to %lld of input", pos);

    if ((ret = read(fd_in, data, size)) < 0)
        pdie("Read %d bytes at %lld", size, pos);

    if (ret != size)
        die("Read %d bytes instead of %d at %lld", ret, size, pos);

    if (llseek(fd_out, pos, SEEK_SET) != pos)
        pdie("Seek to %lld of output", pos);

    if ((ret = write(fd_out, data, size)) < 0)
        pdie("Write %d bytes at %lld", size, pos);

    if (ret != size)
        die("Write %d bytes instead of %d at %lld", ret, size, pos);
}

static void dump_orphaned(DOS_FS *fs)
{
    loff_t clus_offset;
    int i;

    /* check fat entry that is not zero */

    for (i = FAT_START_ENT; i < fs->clusters + 2; i++) {
        if (fs->fat[i].value != 0) {
            clus_offset = CLUSTER_OFFSET(fs, fs->fat[i].value);
            dump_area(clus_offset, fs->cluster_size, buf_clus);
        }
    }
}

static void __traverse_file(DOS_FS *fs, uint32_t clus_num)
{
    uint32_t cluster;
    loff_t clus_offset;
    uint32_t prev_clus = 0;

    cluster = clus_num;

    do {
        clus_offset = CLUSTER_OFFSET(fs, cluster);
        dump_area(clus_offset, fs->cluster_size, buf_clus);

        prev_clus = cluster;
        cluster = NEXT_CLUSTER(fs, cluster);
        fs->fat[prev_clus].value = 0;

    } while (cluster > 0 && cluster < fs->clusters + FAT_START_ENT);
}

static void __traverse_dir(DOS_FS *fs, uint32_t clus_num)
{
    DIR_ENT de;
    DIR_ENT *p_de;
    loff_t clus_offset;
    int offset = 0;
    uint32_t sub_clus;
    uint32_t prev_clus;

    /* clus_num parameter is valid, already checked before being called */
    clus_offset = CLUSTER_OFFSET(fs, clus_num);
    dump_area(clus_offset, fs->cluster_size, buf_clus);

    while (clus_num > 0 && clus_num != -1) {
        if (llseek(fd_in, clus_offset + offset, SEEK_SET) !=
                clus_offset + offset)
            pdie("Seek to %lld of input", clus_offset + offset);

        if (read(fd_in, &de, sizeof(DIR_ENT)) < 0)
            pdie("Read %d bytes at %lld", sizeof(DIR_ENT), clus_offset);

        offset += sizeof(DIR_ENT);
        if (!(offset % fs->cluster_size)) {
            prev_clus = clus_num;
            if ((clus_num = NEXT_CLUSTER(fs, clus_num)) == 0 ||
                    clus_num == -1) {
                fs->fat[prev_clus].value = 0;
                break;
            }

            fs->fat[prev_clus].value = 0;
            clus_offset = CLUSTER_OFFSET(fs, clus_num);
            dump_area(clus_offset, fs->cluster_size, buf_clus);
        }

        if (IS_FREE(de.name) || IS_LFN_ENT(de.attr) ||
                IS_VOLUME_LABEL(de.attr) ||
                !strncmp((char *)de.name, MSDOS_DOT, LEN_FILE_NAME) ||
                !strncmp((char *)de.name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
            continue;
        }

        p_de = &de;
        sub_clus = DE_START_CLUSTER(fs, p_de);
        if (sub_clus > 0 && sub_clus != -1) {
            traverse_tree(fs, sub_clus, de.attr);
        }
    }
}

static void traverse_tree(DOS_FS *fs, uint32_t clus_num, int attr)
{
    if (attr & ATTR_DIR) {
        /* directory */
        __traverse_dir(fs, clus_num);
    } else if (attr & ATTR_ARCH) {
        /* file */
        __traverse_file(fs, clus_num);
    }
}

static void dump_data(DOS_FS *fs)
{
    loff_t clus_offset;
    uint32_t clus_num;
    int offset = 0;
    int i;
    DIR_ENT de;
    DIR_ENT *p_de;

    /* dump root cluster */
    if (fs->root_cluster) {
        clus_offset = CLUSTER_OFFSET(fs, fs->root_cluster);
    }
    else {
        clus_offset = fs->root_start;
    }

    offset = clus_offset % fs->cluster_size;
    if (offset) {
        die("clus_offset is not valid");
    }

    if (fs->root_cluster) {
        traverse_tree(fs, fs->root_cluster, ATTR_DIR);
    }
    else {
        dump_area(clus_offset, fs->cluster_size, buf_clus);

        for (i = 0; i < fs->root_entries; i++) {
            if (llseek(fd_in, clus_offset + i * sizeof(DIR_ENT), SEEK_SET) !=
                    clus_offset + i * sizeof(DIR_ENT)) {
                pdie("Seek to %lld of input",
                        clus_offset + i * sizeof(DIR_ENT));
            }

            if (read(fd_in, &de, sizeof(DIR_ENT)) < 0) {
                pdie("Read %d bytes at %lld", sizeof(DIR_ENT),
                        clus_offset + i * sizeof(DIR_ENT));
            }

            if (IS_FREE(de.name) || IS_LFN_ENT(de.attr) ||
                    IS_VOLUME_LABEL(de.attr) ||
                    !strncmp((char *)de.name, MSDOS_DOT, LEN_FILE_NAME) ||
                    !strncmp((char *)de.name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
                continue;
            }

            p_de = &de;
            clus_num = DE_START_CLUSTER(fs, p_de);
            if (clus_num > 0 && clus_num != -1) {
                traverse_tree(fs, clus_num, de.attr);
            }
        }
    }
}

/* It's for dosfsdump, read just one fat 1st or selected. */
void __read_fat_dump(DOS_FS *fs)
{
    void *fat;
    int size;
    int i;
    loff_t fat_offset = 0;

    /* 2 == FAT_START_ENT */
    size = ((fs->clusters + 2ULL) * fs->fat_bits + 7) / 8ULL;
    fat = alloc_mem(size);

    fat_offset = fs->fat_start;
    if (fat_num && fat_num < fs->nfats) {
        fat_offset = fs->fat_start + fs->fat_size * fat_num;
    }

    if (llseek(fd_in, fat_offset, SEEK_SET) != fat_offset)
        pdie("Seek to %lld of input", fat_offset);

    if (read(fd_in, fat, size) < 0)
        pdie("Read %d bytes at %lld", size, fat_offset);

    fs->fat = alloc_mem(sizeof(FAT_ENTRY) * (fs->clusters + 2ULL));

    for (i = 0; i < FAT_START_ENT; i++) {
        GET_FAT_ENTRY(&fs->fat[i], fat, i, fs);
    }

    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        GET_FAT_ENTRY(&fs->fat[i], fat, i, fs);

        if (fs->fat[i].value >= fs->clusters + FAT_START_ENT &&
                (fs->fat[i].value < FAT_MIN_BAD(fs))) {
            printf("Cluster %u out of range (%u > %u). Setting to EOF.\n",
                    i, fs->fat[i].value, fs->clusters + FAT_START_ENT - 1);
            fs->fat[i].value = 0;
        }
    }

    free_mem(fat);
}

static void dump_fats(DOS_FS *fs)
{
    int i, j;

    if (llseek(fd_in, fs->fat_start, SEEK_SET) != fs->fat_start)
        pdie("Seek to %lld of input", fs->fat_start);

    if (llseek(fd_out, fs->fat_start, SEEK_SET) != fs->fat_start)
        pdie("Seek to %lld of input", fs->fat_start);

    for (i = 0; i < fs->nfats; i++) {
        for (j = 0; j < sec_per_fat; j++) {
            if (read(fd_in, buf_sec, sector_size) < 0)
                pdie("Read FAT");

            if (write(fd_out, buf_sec, sector_size) < 0)
                pdie("Write FAT");
        }
    }

    __read_fat_dump(fs);
}

static void dump_reserved(DOS_FS *fs)
{
    int i;

    if (llseek(fd_in, 0, SEEK_SET) != 0)
        pdie("Seek to %lld of input", 0);

    if (llseek(fd_out, 0, SEEK_SET) != 0)
        pdie("Seek to %lld of input", 0);

    for (i = 0; i < reserved_cnt; i++) {
        if (read(fd_in, buf_sec, sector_size) < 0)
            pdie("Read reserved sector");

        if (write(fd_out, buf_sec, sector_size) < 0)
            pdie("Write reserved sector");
    }
}

static int read_boot_dump(DOS_FS *fs, struct boot_sector *b)
{
    unsigned int total_sectors;
    unsigned short sectors;
    unsigned long long fat_size_bits;
    unsigned long long num_clusters;
    off_t data_size;
    off_t last_offset;
    int ret = 0;
    int change_flag = 0;

    /* read boot_sector */
    if (llseek(fd_in, 0, SEEK_SET) != 0)
        pdie("Seek to %lld of input", 0);

    if (read(fd_in, b, sizeof(struct boot_sector)) < 0)
        pdie("Read %d bytes at %lld", sizeof(struct boot_sector), 0);

    reserved_cnt = CF_LE_W(b->reserved_cnt);
    sector_size = GET_UNALIGNED_W(b->sector_size);
    sec_per_fat = CF_LE_W(b->sec_per_fat) ?
        CF_LE_W(b->sec_per_fat) : CF_LE_L(b->fat32.sec_per_fat32);

    if (!sector_size || (sector_size & (SECTOR_SIZE - 1))) {
        /* set default sector size */
        sector_size = 512;
    }

    fs->cluster_size = b->sec_per_clus * sector_size;
retry:
    if (!fs->cluster_size) {
        /* Can't dump all blocks, just dump reserved sectors only */
        b->sec_per_clus = 4;
        fs->cluster_size = b->sec_per_clus * sector_size;
        dump_flag = DUMP_RESERVED;
        change_flag = 1;
    }

    fs->fat_size = sec_per_fat * sector_size;

    fs->nfats = b->nfats;
    sectors = GET_UNALIGNED_W(b->sectors);

    total_sectors = sectors ? sectors : CF_LE_L(b->total_sect);

    /* Can't access last odd sector anyway, so round down */
    last_offset = (off_t)((total_sectors & ~1) - 1) * (off_t)sector_size;
    ret = llseek(fd_in, last_offset, SEEK_SET);
    if (ret != last_offset) {
        /* Can't dump all blocks, just dump reserved and FAT only */
        dump_flag = DUMP_FAT;
    }

    fs->fat_start = (off_t)reserved_cnt * sector_size;
    fs->root_start = fs->fat_start + (b->nfats * sec_per_fat * sector_size);
    fs->root_entries = GET_UNALIGNED_W(b->dir_entries);
    fs->data_start = fs->root_start +
        ROUND_TO_MULTIPLE(fs->root_entries << MSDOS_DIR_BITS, sector_size);
    data_size = (off_t)total_sectors * sector_size - fs->data_start;
    fs->clusters = data_size / fs->cluster_size;
    fs->root_cluster = 0;   /* indicates standard, pre-FAT32 root dir */
    fs->fsinfo_start = 0;   /* no FSINFO structure */
    fs->free_clusters = -1; /* unknown */

    if (!b->sec_per_fat && b->fat32.sec_per_fat32) {
        fs->fat_bits = 32;
        fs->root_cluster = CF_LE_L(b->fat32.root_cluster);
        if (!fs->root_cluster && fs->root_entries) {
            /* do nothing */
        }
        else if (!fs->root_cluster && !fs->root_entries) {
            dump_flag = DUMP_FAT;
        }
        else if (fs->root_cluster && fs->root_entries) {
            /* do nothing */
        }

        fs->backupboot_start = CF_LE_W(b->fat32.backup_boot) * sector_size;
        /* TODO: if main boot sector is corrupted, use backup boot */
//        check_backup_boot(fs, &b, sector_size);
    }
    else if (!atari_format) {
        /* On real MS-DOS, a 16 bit FAT is used whenever there would be too
         * much clusers otherwise. */
        fs->fat_bits = (fs->clusters > MSDOS_FAT12) ? 16 : 12;
    }
    else {
        unsigned int device_no;
        struct stat st;

        if (fstat(fd_in, &st) < 0)
            pdie("fstat error");

        device_no = S_ISBLK(st.st_mode) ? (st.st_rdev >> 8) & 0xff : 0;

        /* On Atari, things are more difficult: GEMDOS always uses 12bit FATs
         * on floppies, and always 16 bit on harddisks. */
        fs->fat_bits = 16; /* assume 16 bit FAT for now */

        /* If more clusters than fat entries in 16-bit fat, we assume
         * it's a real MSDOS FS with 12-bit fat. */
        if (fs->clusters + 2 > sec_per_fat * sector_size * 8 / 16 ||
                /* if it's a floppy disk --> 12bit fat */
                device_no == 2 ||
                /* if it's a ramdisk or loopback device and has one of the usual
                 * floppy sizes -> 12bit FAT  */
                ((device_no == 1 || device_no == 7) &&
                 (total_sectors == 720 || total_sectors == 1440 ||
                  total_sectors == 2880)))
            fs->fat_bits = 12;
    }

    /* On FAT32, the high 4 bits of a FAT entry are reserved */
    fs->eff_fat_bits = (fs->fat_bits == 32) ? 28 : fs->fat_bits;

    fat_size_bits = (unsigned long long)fs->fat_size * 8;
    num_clusters = (fat_size_bits / fs->fat_bits) - 2;

    while (fs->clusters > num_clusters) {
        /* check whether sec_per_clus is corrupted and set intentionally,
         * if it is, calculate again */
        if (change_flag) {
            b->sec_per_clus <<= 1;
            fs->cluster_size = b->sec_per_clus * sector_size;
            change_flag = 0;
            goto retry;
        }
        else {
            dump_flag = DUMP_FAT;
            break;
        }
    }

    if (!fs->root_entries && !fs->root_cluster) {
        dump_flag = DUMP_FAT;
    }

    return 0;
}

static void usage(char *name)
{
    fprintf(stderr, "Usage: %s [-f <fat number>] [-o <none>] [-v] device\n", name);
    fprintf(stderr,
            "  -f <fat number>  FAT number to traverse cluster chain\n");
    fprintf(stderr,
            "  -v               verbose mode\n");
    exit(2);
}

void clean_dump(DOS_FS *fs)
{
    if (fs->fat)
        free_mem(fs->fat);
}

int main(int argc, char *argv[])
{
    DOS_FS fs;
    struct boot_sector b;
    int c;
    int ret = 0;

    check_atari(&atari_format);

    while ((c = getopt(argc, argv, "f:o:v")) != EOF) {
        switch (c) {
            case 'f':
                fat_num = atoi(optarg);
                /* dump data using n-th FAT */
                break;
            case 'o':
                break;
            case 'v':
                verbose = 1;
                printf("dosfsdump " VERSION " (" VERSION_DATE ")\n");
                break;
            default:
                usage(argv[0]);
                break;
        }
    }

    if (optind != argc - 1) {
        usage(argv[0]);
    }

    fd_in = open(argv[optind], O_RDONLY);
    if (fd_in < 0) {
        printf("Can't open device('%s')\n", argv[optind]);
        exit(-1);
    }

    fd_out = open(DUMP_FILENAME, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd_out < 0) {
        printf("Can't open output file('%s')\n", DUMP_FILENAME);
        exit(-1);
    }

    ret = read_boot_dump(&fs, &b);
    if (ret) {
        /* TODO */
        //__read_boot_dump(&fs);
    }

    buf_sec = alloc_mem(sector_size);
    if (!buf_sec) {
        die("Memory allocation failed(%s,%d)", __func__, __LINE__);
    }

    /* dump reserved sectors(include boot sector / fsinfo */
    dump_reserved(&fs);

    if (dump_flag <= DUMP_RESERVED) {
        printf("Dump reserved sectors only!\n");
        exit(0);
    }

    dump_fats(&fs);

    free_mem(buf_sec);

    if (dump_flag <= DUMP_FAT) {
        printf("Dump reserved sectors and FATs only!\n");
        exit(0);
    }

    /* default buffer size is cluster size */
    buf_clus = alloc_mem(fs.cluster_size);
    if (!buf_clus) {
        die("Memory allocation failed(%s,%d)", __func__, __LINE__);
    }

    dump_data(&fs);
    dump_orphaned(&fs);

    free_mem(buf_clus);

    clean_dump(&fs);

}
