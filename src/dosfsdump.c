/* SPDX-License-Identifier : GPL-2.0 */

/* dosfsdump.c  -  User interface */

#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "common.h"
#include "dosfs.h"

#define DUMP_FILENAME   "./dump.file"

#define DE_START_CLUSTER(fs, de) \
    ((uint32_t)CF_LE_W(de->start) | \
     (fs->fat_bits == 32 ? CF_LE_W(de->starthi) << 16 : 0))

typedef enum dflag {
    DUMP_RESERVED,  /* Dump reserved sectors only */
    DUMP_FAT,       /* Dump reserved sectors and FATs only */
    DUMP_META,      /* Dump reserved sectors and FAT and Meta only */
    DUMP_ALL,       /* Dump ALL (include data) */
} dflag_t;

extern int errno;

int fd_in;
int fd_out;
int verbose = 0;
int fat_num = 0;
int atari_format = 0;
dflag_t dump_flag = DUMP_META;
unsigned short reserved_cnt;
unsigned short sector_size;
unsigned short sec_per_fat;
uint32_t max_clus_num;  /* Not used, for removing compile error */

char *buf_clus = NULL;
char *buf_sec = NULL;
char outfile[256];

static void traverse_tree(DOS_FS *fs, uint32_t clus_num, int attr);

/* same function for dosfsdump */
static inline void dump__get_fat(DOS_FS *fs, uint32_t cluster, uint32_t *value)
{
    loff_t offset;
    int clus_size;

    switch (fs->fat_bits) {
        case 12: {
            unsigned char data[2] = {0, };

            clus_size = 2;
            offset = fs->fat_start + cluster * 3 / 2;
            if (pread(fd_in, data, clus_size, offset) < 0)
                pdie("Read %d bytes at %lld(%d,%s)", clus_size, offset, __LINE__, __func__);

            *value = 0xfff & (cluster & 1 ? (data[0] >> 4) | (data[1] << 4) :
                    (data[0] | data[1] << 8));
            break;
        }
        case 16: {
            unsigned short data = 0;

            clus_size = 2;
            offset = fs->fat_start + cluster * clus_size;
            if (pread(fd_in, (void *)&data, clus_size, offset) < 0)
                pdie("Read %d bytes at %lld(%d,%s)", clus_size, offset, __LINE__, __func__);

            *value = CF_LE_W(data);
            break;
        }
        case 32: {
            uint32_t data = 0;

            clus_size = 4;
            offset = fs->fat_start + cluster * clus_size;
            if (pread(fd_in, (void *)&data, clus_size, offset) < 0)
                pdie("Read %d bytes at %lld(%d,%s)", clus_size, offset, __LINE__, __func__);

            /* According to MS, the high 4 bits of a FAT32 entry are reserved and
             * are not part of the cluster number. So we cut them off. */
            data = CF_LE_L(data);
            *value = data & 0x0fffffff;
            break;
        }
        default:
            die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }

}

static inline loff_t dump__cluster_start(DOS_FS *fs, uint32_t clus_num)
{
    return fs->data_start +
        ((loff_t)clus_num - FAT_START_ENT) * (unsigned long long)fs->cluster_size;
}

static inline uint32_t dump__next_cluster(DOS_FS *fs, uint32_t clus_num)
{
    uint32_t value;

    dump__get_fat(fs, clus_num, &value);
    if (FAT_IS_BAD(fs, value))
        return -1;
    else
        return FAT_IS_EOF(fs, value) ? -1 : value;
}
/**/

static void dump_area(loff_t pos, int size, void *data)
{
    int ret = 0;

    if ((ret = pread(fd_in, data, size, pos)) < 0)
        pdie("Read %d bytes at %lld", size, pos);

    if (ret != size)
        die("Read %d bytes instead of %d at %lld(%d,%s)", ret, size, pos, __LINE__, __func__);

    if ((ret = pwrite(fd_out, data, size, pos)) < 0)
        pdie("Write %d bytes at %lld(%d,%s)", size, pos, __LINE__, __func__);

    if (ret != size)
        die("Write %d bytes instead of %d at %lld(%d,%s)", ret, size, pos, __LINE__, __func__);
}

static void dump_orphaned(DOS_FS *fs)
{
    loff_t clus_offset;
    int i;
    uint32_t next_clus;

    /* check fat entry that is not zero */

    for (i = 0; i < (fs->bitmap_size / sizeof(long)); i++) {
        fs->real_bitmap[i] ^= fs->bitmap[i];
    }

    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {

        if (i % BITS_PER_LONG == 0 && fs->real_bitmap[i / BITS_PER_LONG] == 0) {
            i = ((i / BITS_PER_LONG) * BITS_PER_LONG) + BITS_PER_LONG - 1;
            continue;
        }

        if (!test_bit(i, fs->real_bitmap)) {
            continue;
        }

        dump__get_fat(fs, i, &next_clus);
        if (next_clus && next_clus < fs->clusters + FAT_START_ENT) {
            clus_offset = dump__cluster_start(fs, next_clus);
            dump_area(clus_offset, fs->cluster_size, buf_clus);
        }
    }
}

static void __traverse_file(DOS_FS *fs, uint32_t clus_num)
{
    uint32_t cluster;
    loff_t clus_offset;

    cluster = clus_num;

    do {
        if (test_bit(cluster, fs->real_bitmap))
            break;
        set_bit(cluster, fs->real_bitmap);

        clus_offset = dump__cluster_start(fs, cluster);
        dump_area(clus_offset, fs->cluster_size, buf_clus);

        cluster = dump__next_cluster(fs, cluster);

    } while (cluster != -1 && cluster < fs->clusters + FAT_START_ENT);
}

static void __traverse_dir(DOS_FS *fs, uint32_t clus_num)
{
    DIR_ENT de;
    DIR_ENT *p_de;
    loff_t clus_offset;
    int offset = 0;
    uint32_t sub_clus;

    set_bit(clus_num, fs->real_bitmap);

    /* clus_num parameter is valid, already checked before being called */
    clus_offset = dump__cluster_start(fs, clus_num);
    dump_area(clus_offset, fs->cluster_size, buf_clus);

    while (clus_num > 0 && clus_num < fs->clusters + FAT_START_ENT) {

        if (pread(fd_in, &de, sizeof(DIR_ENT), clus_offset + offset) < 0)
            pdie("Read %d bytes at %lld(%d,%s)",
                    sizeof(DIR_ENT), clus_offset, __LINE__, __func__);

        offset += sizeof(DIR_ENT);
        if (!(offset % fs->cluster_size)) {
            if ((clus_num = dump__next_cluster(fs, clus_num)) == 0 ||
                    clus_num == -1) {
                break;
            }

            offset = 0;
            if (test_bit(clus_num, fs->real_bitmap))
                continue;
            set_bit(clus_num, fs->real_bitmap);

            clus_offset = dump__cluster_start(fs, clus_num);
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
        if (sub_clus > 0 && sub_clus < fs->clusters + FAT_START_ENT) {
            traverse_tree(fs, sub_clus, de.attr);
        }
    }
}

static void traverse_tree(DOS_FS *fs, uint32_t clus_num, int attr)
{
    if (IS_DIR(attr)) {
        /* directory */
        __traverse_dir(fs, clus_num);
    } else if (IS_FILE(attr)) {
        /* file */
        if (dump_flag == DUMP_ALL) {
            __traverse_file(fs, clus_num);
        }
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
        clus_offset = dump__cluster_start(fs, fs->root_cluster);
    }
    else {
        clus_offset = fs->root_start;
    }

    offset = clus_offset % fs->cluster_size;
    if (offset) {
        fprintf(stderr, "WARN: root cluster does not aligned cluster size\n");
    }

    if (fs->root_cluster) {
        traverse_tree(fs, fs->root_cluster, ATTR_DIR);
    }
    else {
        dump_area(clus_offset, fs->cluster_size, buf_clus);

        for (i = 0; i < fs->root_entries; i++) {
            if (pread(fd_in, &de, sizeof(DIR_ENT),
                        clus_offset + i * sizeof(DIR_ENT)) < 0) {
                pdie("Read %d bytes at %lld(%d,%s)", sizeof(DIR_ENT),
                        clus_offset + i * sizeof(DIR_ENT), __LINE__, __func__);
            }

            if (IS_FREE(de.name) || IS_LFN_ENT(de.attr) ||
                    IS_VOLUME_LABEL(de.attr) ||
                    !strncmp((char *)de.name, MSDOS_DOT, LEN_FILE_NAME) ||
                    !strncmp((char *)de.name, MSDOS_DOTDOT, LEN_FILE_NAME)) {
                continue;
            }

            p_de = &de;
            clus_num = DE_START_CLUSTER(fs, p_de);
            if (clus_num > 0 && clus_num < fs->clusters + FAT_START_ENT) {
                traverse_tree(fs, clus_num, de.attr);
            }
        }
    }
}

/* It's for dosfsdump, read just one fat 1st or selected. */
static void dump__read_fat(DOS_FS *fs)
{
    int fat_size;
    int read_size;
    loff_t offset = 0;
    loff_t start_offset = 0;
    int remain_size;
    int num_cluster;
    int total_cluster = 0;
    char *fat = NULL;
    uint32_t clus_num;
    uint32_t start = FAT_START_ENT; /* skip 0 and 1-th cluster of FAT */

    /* 2 represents FAT_START_ENT */
    fat_size = ((fs->clusters + 2ULL) * fs->fat_bits + 7) / BITS_PER_BYTE;
    fs->bitmap_size = (fat_size + 7) / BITS_PER_BYTE;

    read_size = min(FAT_BUF, fat_size);
    fat = alloc_mem(read_size);

    remain_size = fat_size;

    start_offset = fs->fat_start;
    if (fat_num && fat_num < fs->nfats) {
        start_offset = fs->fat_start + fs->fat_size * fat_num;
    }

    fs->bitmap = alloc_mem(fs->bitmap_size);
    fs->real_bitmap = alloc_mem(fs->bitmap_size);

    while (remain_size > 0) {
        int i;

        if (pread(fd_in, fat, read_size, start_offset + offset) < 0)
            pdie("Read %d bytes at %lld(%d,%s)",
                    read_size, start_offset + offset, __LINE__, __func__);

        num_cluster = read_size / fs->fat_bits;
        for (i = start; i < num_cluster; i++) {
            if (total_cluster + i >= max_clus_num) {
                break;
            }

            dump__get_fat(fs, total_cluster + i, &clus_num);
            if (!clus_num)
                continue;

            if (clus_num >= fs->clusters + FAT_START_ENT &&
                    clus_num < FAT_MIN_BAD(fs)) {
                fprintf(stderr, "WARN: Cluster %u out of range (%u > %u). Setting to EOF.\n",
                        i, clus_num, fs->clusters + FAT_START_ENT - 1);
                continue;
            }

            /* set bitmap only valid cluster */
            set_bit(total_cluster + i, fs->bitmap);
        }

        start = 0;
        total_cluster += num_cluster;
        remain_size -= read_size;
        if (remain_size < read_size)
            read_size = remain_size;
    }

    free_mem(fat);
}

static void dump_fats(DOS_FS *fs)
{
    int i, j;

    if (lseek(fd_in, fs->fat_start, SEEK_SET) != fs->fat_start)
        pdie("Seek to %lld of input(%d,%s)", fs->fat_start, __LINE__, __func__);

    if (lseek(fd_out, fs->fat_start, SEEK_SET) != fs->fat_start)
        pdie("Seek to %lld of input(%d,%s)", fs->fat_start, __LINE__, __func__);

    for (i = 0; i < fs->nfats; i++) {
        for (j = 0; j < sec_per_fat; j++) {
            if (read(fd_in, buf_sec, sector_size) < 0)
                pdie("Read FAT(%d,%s)", __LINE__, __func__);

            if (write(fd_out, buf_sec, sector_size) < 0)
                pdie("Write FAT(%d,%s)", __LINE__, __func__);
        }
    }

    dump__read_fat(fs);
}

static void dump_reserved(DOS_FS *fs)
{
    int i;

    if (lseek(fd_in, 0, SEEK_SET) != 0)
        pdie("Seek to %lld of input(%d,%s)", 0, __LINE__, __func__);

    if (lseek(fd_out, 0, SEEK_SET) != 0)
        pdie("Seek to %lld of input(%d,%s)", 0, __LINE__, __func__);

    for (i = 0; i < reserved_cnt; i++) {
        if (read(fd_in, buf_sec, sector_size) < 0)
            pdie("Read reserved sector(%d,%s)", __LINE__, __func__);

        if (write(fd_out, buf_sec, sector_size) < 0)
            pdie("Write reserved sector(%d,%s)", __LINE__, __func__);
    }
}

static inline int is_valid_media(unsigned char media)
{
    return 0xf8 <= media || media == 0xf0;
}

static int is_valid_fat(DOS_FS *fs, struct boot_sector *b)
{
    unsigned short reserved_sector;

    reserved_sector = CF_LE_W(b->reserved_cnt);
    if (!reserved_sector || !b->nfats || !is_valid_media(b->media))
        return 0;

    return 1;
}

static int dump__read_boot(DOS_FS *fs, struct boot_sector *b)
{
    unsigned int total_sectors;
    unsigned short sectors;
    unsigned long long fat_size_bits;
    unsigned long long num_clusters;
    off_t data_size;
    off_t last_offset;
    off_t device_size;
    off_t ret = 0;
    int change_flag = 0;

    /* read device size */
    device_size = lseek(fd_in, 0, SEEK_END);
    if (device_size < 0) {
        fprintf(stderr, "lseek error (%s)\n", strerror(errno));
        exit(-1);
    }

    if (lseek(fd_out, device_size, SEEK_SET) != device_size) {
        fprintf(stderr, "lseek error (%s)\n", strerror(errno));
        exit(-1);
    }

    if (write(fd_out, "0", 1) < 0) {
        fprintf(stderr, "write error (%s)\n", strerror(errno));
        exit(-1);
    };

    /* read boot_sector */
    if (pread(fd_in, b, sizeof(struct boot_sector), 0) < 0)
        pdie("Read %d bytes at %lld(%d,%s)",
                sizeof(struct boot_sector), 0, __LINE__, __func__);

    if (!is_valid_fat(fs, b)) {
        fprintf(stderr, "Device(or file) is not valid FAT filesystem\n");
        exit(-1);
    }

    if (b->boot_sign != CT_LE_W(BOOT_SIGN)) {
        fprintf(stderr, "Filesystem does not have FAT32 magic number(0x%d)\n", CF_LE_W(b->boot_sign));
        exit(-1);
    }

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
    last_offset = (off_t)((total_sectors & ~0x01) - 1) * (off_t)sector_size;
    ret = lseek(fd_in, last_offset, SEEK_SET);
    if (ret != last_offset) {
        /* Can't dump all blocks, just dump reserved and FAT only */
        dump_flag = DUMP_FAT;
    }

    fs->fat_start = (off_t)reserved_cnt * sector_size;
    fs->root_start = fs->fat_start + (b->nfats * (off_t)sec_per_fat * sector_size);
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
        if (fs->clusters + FAT_START_ENT > sec_per_fat * sector_size * 8 / 16 ||
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
    fprintf(stderr, "Usage: %s [-o <output file path>] [-f <fat number>] [-v] [-h] device\n", name);
    fprintf(stderr,
            "  -o <output file path>    help message\n");
    fprintf(stderr,
            "  -f <fat number>          FAT number to traverse cluster chain\n");
    fprintf(stderr, "  -v                       verbose mode\n");
    fprintf(stderr, "  -h                       help message\n");
}

void clean_dump(DOS_FS *fs)
{
    if (fs->bitmap)
        free_mem(fs->bitmap);

    if (fs->real_bitmap)
        free_mem(fs->real_bitmap);
}

int main(int argc, char *argv[])
{
    DOS_FS fs;
    struct boot_sector b;
    int c;
    int ret = 0;

    check_atari(&atari_format);

    memset(outfile, 0, 256);
    memcpy(outfile, DUMP_FILENAME, strlen(DUMP_FILENAME));
    while ((c = getopt(argc, argv, "f:o:vh")) != EOF) {
        switch (c) {
            case 'f':
                fat_num = atoi(optarg);
                /* dump data using n-th FAT */
                break;
            case 'o':   /* specify output file */
                memset(outfile, 0, 255);
                if (strlen(optarg) > 255) {
                    fprintf(stderr, "!! Output filename length is longer than 255\n");
                    usage(argv[0]);
                    exit(EXIT_SYNTAX_ERROR);
                }
                memcpy(outfile, optarg, strlen(optarg));
                break;
            case 'v':
                verbose = 1;
                fprintf(stderr, "dosfsdump " VERSION " (" VERSION_DATE ")\n");
                break;
            case 'd':
                dump_flag = DUMP_ALL;
                break;
            case 'h':
                usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                usage(argv[0]);
                exit(EXIT_SYNTAX_ERROR);
        }
    }

    if (optind != argc - 1) {
        usage(argv[0]);
        exit(EXIT_SYNTAX_ERROR);
    }

    fd_in = open(argv[optind], O_RDONLY);
    if (fd_in < 0) {
        fprintf(stderr, "Can't open device('%s')\n", argv[optind]);
        exit(EXIT_FAILURE);
    }

    fd_out = open(outfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd_out < 0) {
        fprintf(stderr, "Can't open output file('%s')\n", outfile);
        exit(EXIT_FAILURE);
    }

    ret = dump__read_boot(&fs, &b);
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
        fprintf(stderr, "Dump reserved sectors only!\n");
        exit(EXIT_SUCCESS);
    }

    dump_fats(&fs);

    free_mem(buf_sec);

    if (dump_flag <= DUMP_FAT) {
        fprintf(stderr, "Dump reserved sectors and FATs only!\n");
        exit(EXIT_SUCCESS);
    }

    /* default buffer size is cluster size */
    buf_clus = alloc_mem(fs.cluster_size);
    if (!buf_clus) {
        die("Memory allocation failed(%s,%d)", __func__, __LINE__);
    }

    dump_data(&fs);
    if (dump_flag == DUMP_ALL) {
        dump_orphaned(&fs);
    }

    free_mem(buf_clus);

    clean_dump(&fs);

    close(fd_in);
    close(fd_out);
    fprintf(stderr, "Done: dump \"%s\" to \"%s\"\n", argv[optind], outfile);
}
