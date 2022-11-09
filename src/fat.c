/* SPDX-License-Identifier : GPL-2.0 */

/* fat.c  -  Read/write access to the FAT */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "check.h"
#include "fat.h"
#include "file.h"

uint32_t alloc_clusters;

int __check_file_owner(DOS_FS *fs, uint32_t start, uint32_t cluster, int cnt);
void set_exclusive_bitmap(DOS_FS *fs)
{
    unsigned int i;

    /* bitmap : read from disk(FAT)
     * real_bitmap : set by traversing file tree
     * reclaim_bitmap : not used */

    memcpy(fs->reclaim_bitmap, fs->real_bitmap, fs->bitmap_size);

    /* After exclusive OR operation,
     * remained bits represent orphaned clusters */
    for (i = 0; i < (fs->bitmap_size / sizeof(long)); i++) {
        fs->real_bitmap[i] ^= fs->bitmap[i];
    }

    memcpy(fs->bitmap, fs->reclaim_bitmap, fs->bitmap_size);
    memset(fs->reclaim_bitmap, 0, fs->bitmap_size);

    /* bitmap : real_bitmap backup
     * real_bitmap : orphan clusters set
     * reclaim_bitmap : zero cleared for reclaimed cluster */
}

static void init_fat_cache(DOS_FS *fs)
{
    loff_t aligned_offset;

    /* initialize fat cache */
    fs->fat_cache.start = -1;

    aligned_offset = fs->fat_start & ~(sysconf(_SC_PAGE_SIZE) - 1);
    fs->fat_cache.diff = fs->fat_start - aligned_offset;

    fs->fat_cache.cpc = FAT_CACHE_SIZE / fs->fat_bits * BITS_PER_BYTE;
    fs->fat_cache.first_cpc =
        (FAT_CACHE_SIZE - fs->fat_cache.diff) / fs->fat_bits * BITS_PER_BYTE;
    fs->fat_cache.last_cpc =
        ((fs->clusters + FAT_START_ENT) - fs->fat_cache.first_cpc) %
        fs->fat_cache.cpc;

    fs->fat_cache.addr = NULL;
}

void read_fat(DOS_FS *fs)
{
    int fat_size;
    int read_size;
    loff_t offset = 0;
    int remain_size;
    int bitmap_size;
    int first_ok;
    int second_ok;
    int clus_size;  /* cluster size (bytes) */
    int cpr;    /* number of cluster per read size */
    int total_cluster = 0;
    fat_select_t flag = FAT_NONE;
    char *first_fat = NULL;
    char *second_fat = NULL;
    uint32_t clus_num;
    uint32_t start = FAT_START_ENT; /* skip 0, 1-th cluster of FAT */

    /* 2 represent FAT_START_ENT */
    fat_size = ((fs->clusters + 2ULL) * fs->fat_bits + 7) / BITS_PER_BYTE;
    fs->bitmap_size = bitmap_size = (fat_size + 7) / BITS_PER_BYTE;

    clus_size = fs->fat_bits / BITS_PER_BYTE;
    read_size = min(FAT_BUF, fat_size);
    first_fat = alloc_mem(read_size);
    if (fs->nfats > 1) {
        second_fat = alloc_mem(read_size);
    }
    remain_size = fat_size;

    /* TODO: handle in case that fs->nfats is bigger than 2 */
    if (fs->nfats > 2) {
        printf("Not support filesystem that have more than 2 FATs\n");
    }

    /* make bitmap from selected FAT */
    fs->bitmap = qalloc(&mem_queue, bitmap_size);
    fs->real_bitmap = qalloc(&mem_queue, bitmap_size);
    fs->reclaim_bitmap = qalloc(&mem_queue, bitmap_size);

    init_fat_cache(fs);

    /* read FAT with DEFALUT_FAT_BUF size for memory optimization */
    while (remain_size > 0) {
        int i;

        fs_read(fs->fat_start + offset, read_size, first_fat);

        if (second_fat) {
            fs_read(fs->fat_start + fs->fat_size + offset,
                    read_size, second_fat);
        }

        /* in case of first FAT read */
        if (offset == 0) {
            uint32_t value;
            uint32_t value2;

            get_fat(fs, FAT_FIRST, &value);
            get_fat(fs, FAT_SECOND, &value2);

            first_ok = (value & FAT_EXTD(fs)) == FAT_EXTD(fs);
            second_ok = (value2 & FAT_EXTD(fs)) == FAT_EXTD(fs);
        }

        if (second_fat && memcmp(first_fat, second_fat, read_size) != 0) {
            if (!first_ok && !second_ok) {
                printf("Both FATs appear to be corrupt. Giving up.\n");
                exit(1);
            }

            if (first_ok && !second_ok) {
                if (flag == FAT_NONE)
                    printf("FATs differ - using first FAT.\n");
                flag = FAT_FIRST;
            }

            if (!first_ok && second_ok) {
                if (flag == FAT_NONE)
                    printf("FATs differ - using second FAT.\n");
                flag = FAT_SECOND;
            }

            if (first_ok && second_ok) {
                if (flag == FAT_NONE) {
                    if (interactive) {
                        printf("FATs differ but appear to be intact. "
                                "Use which FAT ?\n"
                                "1) Use first FAT\n"
                                "2) Use second FAT\n");
                        if (get_key("12", "?") == '1') {
                            flag = FAT_FIRST;
                        } else {
                            flag = FAT_SECOND;
                        }
                    }
                    else {
                        printf("FATs differ but appear to be intact. "
                                "Using first FAT.\n");
                        flag = FAT_FIRST;
                    }
                }
            }

            if (flag == FAT_SECOND) {
                /* TODO: how about writing immediately for FAT ?? */
                fs_write(fs->fat_start + offset, read_size, second_fat);

                /* TODO: memcpy is needed? just point second_fat isn't enough? */
                memcpy(first_fat, second_fat, read_size);
            }
            else {
                /* TODO: how about writing immediately for FAT ?? */
                fs_write(fs->fat_start + fs->fat_size + offset,
                        read_size, first_fat);
            }
        }

        cpr = read_size / clus_size;
        for (i = start; i < cpr; i++) {
            get_fat(fs, total_cluster + i, &clus_num);
            if (!clus_num)
                continue;

            if (clus_num >= fs->clusters + FAT_START_ENT &&
                    clus_num < FAT_MIN_BAD(fs)) {
                printf("Cluster %u out of range (%u > %u). Setting to EOF.\n",
                        i, clus_num, fs->clusters + FAT_START_ENT - 1);
                set_fat(fs, total_cluster + i, -1);
                set_bit(total_cluster + i, fs->bitmap);
                continue;
            }

            /* TODO: handling in case that clus_num is bad cluster */

            /* set bitmap only valid cluster */
            set_bit(total_cluster + i, fs->bitmap);
        }

        start = 0;
        total_cluster += cpr;
        offset += read_size;

        remain_size -= read_size;
        if (remain_size < read_size)
            read_size = remain_size;
    }

    if (second_fat) {
        free_mem(second_fat);
    }

    free_mem(first_fat);
}

/* read_fat_cache apply only FAT32 */
static void read_fat_cache(DOS_FS *fs, uint32_t cluster)
{
    loff_t mmap_offset;
    loff_t aligned_offset;

    if (cluster > fs->clusters + FAT_START_ENT) {
        die("cluster number is more than max cluster number\n");
    }

    /* fat cache doesn't hit */
    if (!(cluster >= fs->fat_cache.start &&
            cluster < fs->fat_cache.start + fs->fat_cache.cnt)) {
        mmap_offset = fs->fat_start + cluster * fs->fat_bits / BITS_PER_BYTE;
        aligned_offset = mmap_offset & ~(sysconf(_SC_PAGE_SIZE) - 1);

        /* fat_start is already page algined address */
        if (fs->fat_cache.diff == 0) {
            fs->fat_cache.cnt = fs->fat_cache.cpc;
            fs->fat_cache.start = (cluster >= FAT_START_ENT) ?
                ((cluster - 1) / fs->fat_cache.cpc) *  fs->fat_cache.cpc : 0;
        }
        /* fat_start is not page aligned address */
        else {
            /* first cache */
            if (cluster >= 0 && cluster < fs->fat_cache.first_cpc) {
                fs->fat_cache.start = 0;
                fs->fat_cache.cnt = fs->fat_cache.first_cpc;
            }
            /* last cache */
            else if (cluster >=
                    (fs->clusters + FAT_START_ENT) - fs->fat_cache.last_cpc) {
                fs->fat_cache.start =
                    ((cluster - fs->fat_cache.first_cpc) / fs->fat_cache.cpc) *
                    fs->fat_cache.cpc + fs->fat_cache.first_cpc;
                fs->fat_cache.cnt = fs->fat_cache.last_cpc;
            }
            /* others */
            else {
                fs->fat_cache.cnt = fs->fat_cache.cpc;
                fs->fat_cache.start =
                    ((cluster - fs->fat_cache.first_cpc) / fs->fat_cache.cpc) *
                    fs->fat_cache.cpc + fs->fat_cache.first_cpc;
            }
        }

        /* munmap for previous memory mapping and mmap new FAT area
         * that include cluster */
        if (fs->fat_cache.start != -1) {
            fs_munmap(fs->fat_cache.addr, FAT_CACHE_SIZE);
        }

        fs->fat_cache.addr = fs_mmap(NULL, aligned_offset, FAT_CACHE_SIZE);
    }
}

void get_fat(DOS_FS *fs, uint32_t cluster, uint32_t *value)
{
    loff_t offset;
    int clus_size;

    switch (fs->fat_bits) {
        case 12: {
            unsigned char data[2];

            clus_size = 2;
            offset = fs->fat_start + cluster * 3 / 2;
            fs_read(offset, clus_size, data);

            *value = 0xfff & (cluster & 1 ? (data[0] >> 4) | (data[1] << 4) :
                    (data[0] | data[1] << 8));
            break;
        }
        case 16: {
            unsigned short data;

            clus_size = 2;

            /* TODO: is it needed applying fat_cache? */
            offset = fs->fat_start + cluster * clus_size;
            fs_read(offset, clus_size, &data);

            *value = CF_LE_W(data);
            break;
        }
        case 32: {
            uint32_t data;

            clus_size = 4;
#if 1
            read_fat_cache(fs, cluster);

            /* offset in cache */
            offset = ((cluster - fs->fat_cache.start) * clus_size) % FAT_CACHE_SIZE;
            data = *(uint32_t *)(fs->fat_cache.addr + offset);

            /* offset in block device */
            offset = fs->fat_start + cluster * clus_size;
            fs_find_data_copy(offset, clus_size, &data);
#else
            /* offset in device */
            offset = fs->fat_start + cluster * clus_size;
            fs_read(offset, clus_size, &data);
#endif

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

void set_fat(DOS_FS *fs, uint32_t cluster, uint32_t new)
{
    unsigned char data[4];
    loff_t offset;
    int clus_size;
    int i;

    if ((int32_t)new == -1)
        new = FAT_EOF(fs);
    else if ((int32_t)new == -2)
        new = FAT_BAD(fs);

    switch (fs->fat_bits) {
        uint32_t value;

        case 12:

            clus_size = 2;
            offset = fs->fat_start + cluster * 3 / 2;

            if (cluster & 1) {
                get_fat(fs, cluster - 1, &value);

                data[0] = ((new & 0xf) << 4) | (value >> 8);
                data[1] = new >> 4;
            }
            else {
                get_fat(fs, cluster + 1, &value);

                data[0] = new & 0xff;
                data[1] = (new >> 8) |
                    (cluster == fs->clusters - 1 ? 0 : (0xff & value) << 4);
            }
            break;

        case 16:
            clus_size = 2;
            offset = fs->fat_start + cluster * clus_size;
            *(uint16_t *)data = CT_LE_W(new);
            break;

        case 32:
            clus_size = 4;
            offset = fs->fat_start + cluster * clus_size;
            get_fat(fs, cluster, (uint32_t *)data);

            /* According to MS, the high 4 bits of a FAT32 entry are reserved and
             * are not part of the cluster number. So we cut them off. */
            *(uint32_t *)data =
                CT_LE_L((new & 0xfffffff) | (*(uint32_t *)data & 0xf0000000));
            break;

        default:
            die("Bad FAT entry size: %d bits.", fs->fat_bits);
    }

    fs_write(offset, clus_size, &data);
    for (i = 1; i < fs->nfats; i++) {
        fs_write(offset + (fs->fat_size * i), clus_size, &data);
    }
}

int bad_cluster(DOS_FS *fs, uint32_t cluster)
{
    uint32_t value;

    get_fat(fs, cluster, &value);
    return FAT_IS_BAD(fs, value);
}

uint32_t next_cluster(DOS_FS *fs, uint32_t cluster)
{
    uint32_t next_clus;

    get_fat(fs, cluster, &next_clus);
    if (FAT_IS_BAD(fs, next_clus))
        die("Internal error: next_cluster on bad cluster");

    return FAT_IS_EOF(fs, next_clus) ? -1 : next_clus;
}

loff_t cluster_start(DOS_FS *fs, uint32_t cluster)
{
    return fs->data_start +
        ((loff_t)cluster - FAT_START_ENT) * (unsigned long long)fs->cluster_size;
}

inline void inc_alloc_cluster(void)
{
    alloc_clusters++;
}

inline void dec_alloc_cluster(void)
{
    alloc_clusters--;
}

inline void set_bitmap_reclaim(DOS_FS *fs, uint32_t cluster)
{
    set_bit(cluster, fs->reclaim_bitmap);
    alloc_clusters++;
}

inline void clear_bitmap_reclaim(DOS_FS *fs, uint32_t cluster)
{
    clear_bit(cluster, fs->reclaim_bitmap);
    alloc_clusters--;
}

inline void set_bitmap_occupied(DOS_FS *fs, uint32_t cluster)
{
    set_bit(cluster, fs->real_bitmap);
    set_bit(cluster, fs->bitmap);
    alloc_clusters++;
}

inline void clear_bitmap_occupied(DOS_FS *fs, uint32_t cluster)
{
    clear_bit(cluster, fs->real_bitmap);
    clear_bit(cluster, fs->bitmap);
    alloc_clusters--;
}

void fix_bad(DOS_FS *fs)
{
    uint32_t i;
    uint32_t next_clus;

    if (verbose)
        printf("Checking for bad clusters.\n");

    /* use reclaim_bitmap to check bad cluster in this function temporarily */
    memcpy(fs->reclaim_bitmap, fs->real_bitmap, fs->bitmap_size);

    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        if (test_bit(i, fs->reclaim_bitmap)) {
            continue;
        }

        get_fat(fs, i, &next_clus);
        if (!FAT_IS_BAD(fs, next_clus)) {
            if (!fs_test(cluster_start(fs, i), fs->cluster_size)) {
                printf("Cluster %u is unreadable.\n", i);
                set_fat(fs, i, -2);
                /* TODO: check if clear_occupied is needed */
                clear_bitmap_occupied(fs, i);
            }
        }
    }

    memset(fs->reclaim_bitmap, 0, fs->bitmap_size);
}

void reclaim_free(DOS_FS *fs)
{
    int reclaimed;
    uint32_t i;
    uint32_t next_clus;

    if (verbose)
        printf("Checking for unused clusters.\n");

    reclaimed = 0;
    set_exclusive_bitmap(fs);

    /* Do not set bitmap in reclaim routine */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        if (!test_bit(i, fs->real_bitmap)) {
            continue;
        }

        get_fat(fs, i, &next_clus);
        if (next_clus && !FAT_IS_BAD(fs, next_clus)) {
            set_fat(fs, i, 0);
            reclaimed++;
        }
    }

    if (reclaimed)
        printf("Reclaimed %d unused cluster%s (%llu bytes).\n", reclaimed,
                reclaimed == 1 ?  "" : "s",
                (unsigned long long)reclaimed * fs->cluster_size);
}

/* Find start cluster of orphan files.
 * After function call, start clusters are remained as set bit in real_bitmap */
static void find_start_clusters(DOS_FS *fs)
{
    uint32_t prev;
    uint32_t i, walk;
    uint32_t next_clus;
    uint32_t cnt;

    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        if (!test_bit(i, fs->real_bitmap)) {
            continue;
        }

        get_fat(fs, i, &next_clus);
        if (next_clus && !FAT_IS_BAD(fs, next_clus)) {
            prev = i;
            cnt = 1;
            for (walk = next_cluster(fs, i); walk > 0 && walk != -1;
                    walk = next_cluster(fs, walk)) {

                /* broke self cycle case */
                if (prev == walk) {
                    set_fat(fs, prev, -1);
                    break;
                }

                if (test_bit(walk, fs->real_bitmap)) {
                    /* walk is not start cluster, just clear bit */
                    clear_bit(walk, fs->real_bitmap);
                }
                else {
                    set_fat(fs, prev, -1);
                    break;
                }
                prev = walk;
                cnt++;
                if (cnt > fs->clusters) {
                    printf("Orphan cluster(%d) has cluster chain cycle\n", i);
                    break;;
                }
            }
        }
    }
}

void reclaim_file(DOS_FS *fs)
{
    int reclaimed, files;
    uint32_t i, next, walk;

    if (verbose)
        printf("Reclaiming unconnected clusters.\n");

    /* Remove checked cluster,
     * After function called, remained bitmap represent orphan clusters */
    set_exclusive_bitmap(fs);

    /* check if orphan cluster chain has normal cluster.
     * if then, set EOF to orphan cluster's value. */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        uint32_t value;

        /* if bit is set, that cluster is orphan cluster */
        if (!test_bit(i, fs->real_bitmap)) {
            continue;
        }

        get_fat(fs, i, &next);
        if (next && next < fs->clusters + FAT_START_ENT) {
            get_fat(fs, next, &value);
            /* In case that i's next cluster is already in other cluster chain
             * or i's next cluster has wrong cluster value */
            if (!test_bit(next, fs->real_bitmap) ||
                    !value || FAT_IS_BAD(fs, value))
                set_fat(fs, i, -1);
        }
    }

    /* after find_start_clusters(),
     * real_bitmap represent orphan's start cluster */
    find_start_clusters(fs);

    files = reclaimed = 0;
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        /* check real_bitmap, and if it set,
         * it(i) is orphaned cluster's start cluster */
        if (test_bit(i, fs->real_bitmap)) {
            DIR_ENT de;
            loff_t offset;
            uint32_t clus_cnt;
            uint32_t prev;

            files++;
            offset = alloc_rootdir_entry(fs, &de, "FSCK%04dREC");
            de.start = CT_LE_W(i & 0xffff);

            if (fs->fat_bits == 32)
                de.starthi = CT_LE_W(i >> 16);

            set_bitmap_reclaim(fs, i);

            if (list) {
                printf("Reclaimed file %s, start cluster(%d)\n",
                        file_name((unsigned char *)de.name), i);
            }

            /* check circular/shared cluster chain */
            clus_cnt = 1;
            prev = i;
            for (walk = next_cluster(fs, i);
                    walk > 0 && walk < fs->clusters + FAT_START_ENT;
                    walk = next_cluster(fs, walk)) {

                if (test_bit(walk, fs->real_bitmap)) {
                    printf("WARNING: there should be not exist set bit of real_bitmap"
                            " on reclaim cluster chain.\n");
                }

                if (test_bit(walk, fs->reclaim_bitmap)) {
                    set_fat(fs, prev, -1);
                    break;
                }
                prev = walk;
                clus_cnt++;

                set_bitmap_reclaim(fs, walk);
            }

            de.size = CT_LE_L(clus_cnt * fs->cluster_size);
            reclaimed += clus_cnt;

            fs_write(offset, sizeof(DIR_ENT), &de);
        }
    }

    if (reclaimed)
        printf("Reclaimed %d unused cluster%s (%llu bytes) in %d chain%s.\n",
                reclaimed, reclaimed == 1 ? "" : "s",
                (unsigned long long)reclaimed * fs->cluster_size, files,
                files == 1 ? "" : "s");
}

uint32_t update_free(DOS_FS *fs)
{
    uint32_t free = 0;
    int do_set = 0;

#if 0
    uint32_t i;
    uint32_t next;
    int temp_cnt = 0;

    /* TODO: to improve performance like as read_fat() */
    for (i = FAT_START_ENT; i < fs->clusters + FAT_START_ENT; i++) {
        get_fat(fs, i, &next);
        if (!next) {
            ++free;
        }
        else
            temp_cnt++;
    }
    printf("calculated free2(get_fat) %d, alloced clusters %d\n", free, temp_cnt);
#endif

    free = fs->clusters - alloc_clusters;

    if (!fs->fsinfo_start)
        return free;

    if (verbose) {
        printf("Checking free cluster summary.\n");

        printf("Total clusters: %d, Allocated clusters: %d, Free clusters: %d\n",
                fs->clusters, alloc_clusters, fs->clusters - alloc_clusters);
    }

    if (fs->free_clusters >= 0) {
        if (fs->free_clusters == -1) {
            printf("Free cluster summary is not initialized\n");
        }

        if (free != fs->free_clusters) {
            printf("Free cluster summary wrong (%u vs. really %u)\n",
                    fs->free_clusters, free);
            if (interactive)
                printf("1) Correct\n"
                        "2) Don't correct\n");
            else
                printf("  Auto-correcting.\n");

            if (!interactive || get_key("12", "?") == '1')
                do_set = 1;
        }
    }
    else {
        printf("Free cluster summary uninitialized (should be %u)\n", free);
        if (interactive) {
            printf("1) Set it\n"
                    "2) Leave it uninitialized\n");
        }
        else {
            printf("  Auto-setting.\n");
        }

        if (!interactive || get_key("12", "?") == '1')
            do_set = 1;
    }

    if (do_set) {
        fs->free_clusters = free;
        free = CT_LE_L(free);
        fs_write(fs->fsinfo_start + offsetof(struct fsinfo_sector, free_clusters),
                sizeof(free), &free);
    }

    return free;
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
