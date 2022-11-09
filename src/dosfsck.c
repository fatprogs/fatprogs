/* SPDX-License-Identifier : GPL-2.0 */

/* dosfsck.c  -  User interface */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "boot.h"
#include "fat.h"
#include "file.h"
#include "check.h"

int interactive = 0, list = 0, test = 0, verbose = 0, write_immed = 0;
int check_dirty = 0;
int check_dirty_only = 0;
int atari_format = 0;
unsigned n_files = 0;
void *mem_queue = NULL;

uint32_t max_clus_num;

static void usage(char *name)
{
    fprintf(stderr, "usage: %s [-aAflrtvVwy] [-d path -d ...] "
            "[-u path -u ...]\n%15sdevice\n", name, "");
    fprintf(stderr, "  -a       automatically repair the file system\n");
    fprintf(stderr, "  -A       toggle Atari file system format\n");
    fprintf(stderr, "  -C       only check filesystem dirty flag(FAT32/16 only)\n");
    fprintf(stderr, "  -d path  drop that file\n");
    fprintf(stderr, "  -f       salvage unused chains to files\n");
    fprintf(stderr, "  -l       list path names\n");
    fprintf(stderr, "  -n       no-op, check non-interactively without changing\n");
    fprintf(stderr, "  -r       interactively repair the file system\n");
    fprintf(stderr, "  -t       test for bad clusters\n");
    fprintf(stderr, "  -u path  try to undelete that (non-directory) file\n");
    fprintf(stderr, "  -v       verbose mode\n");
    fprintf(stderr, "  -V       perform a verification pass\n");
    fprintf(stderr, "  -w       write changes to disk immediately\n");
    fprintf(stderr, "  -y       same as -a, for compat with other *fsck\n");
    exit(2);
}

int main(int argc, char **argv)
{
    DOS_FS fs;
    int rw, salvage_files, verify, c;
    int ret = 0;
    uint32_t free_clusters;

    salvage_files = verify = 0;
    rw = 1;
    interactive = 1;
    check_atari(&atari_format);

    while ((c = getopt(argc, argv, "AaCd:flnrtu:vVwy")) != EOF) {
        switch (c) {
            case 'A': /* toggle Atari format */
                atari_format = !atari_format;
                break;
            case 'a':
            case 'y':
                rw = 1;
                interactive = 0;
                salvage_files = 1;
                break;
            case 'C':
                check_dirty = 1;
                check_dirty_only = 1;
                interactive = 0;
                break;
            case 'd':
                file_add(optarg, fdt_drop);
                break;
            case 'f':
                salvage_files = 1;
                break;
            case 'l':
                list = 1;
                break;
            case 'n':
                rw = 0;
                interactive = 0;
                break;
            case 'r':
                rw = 1;
                interactive = 1;
                break;
            case 't':
                test = 1;
                break;
            case 'u':
                file_add(optarg, fdt_undelete);
                break;
            case 'v':
                verbose = 1;
                printf("dosfsck " VERSION " (" VERSION_DATE ")\n");
                break;
            case 'V':
                verify = 1;
                break;
            case 'w':
                write_immed = 1;
                break;
            default:
                usage(argv[0]);
        }
    }

    if ((test || write_immed) && !rw) {
        fprintf(stderr, "-t and -w require -a or -r\n");
        exit(2);
    }

    if (optind != argc - 1)
        usage(argv[0]);

    printf("dosfsck " VERSION ", " VERSION_DATE ", FAT32, LFN\n");
    fs_open(argv[optind], rw);
    read_boot(&fs);

    if (verify)
        printf("\nStarting check/repair pass.\n");

    do {
        n_files = 0;
        read_fat(&fs);

        if (check_dirty &&
                ((fs.fat_bits == 32) || (fs.fat_bits == 16))) {
            if (check_dirty_flag(&fs)) {
                if (check_dirty_only) {
                    printf("  Just check filesystem dirty flag, exit!\n");
                    exit(4);
                }
            }
            else {
                printf("  Filesystem dirty flag is clean. exit!\n");
                exit(0);
            }
        }

        ret = scan_root(&fs);
        if (ret) {
            qfree(&mem_queue);
        }
    } while (ret);

    if (test)
        fix_bad(&fs);

    check_volume_label(&fs);

    if (salvage_files)
        reclaim_file(&fs);
    else
        reclaim_free(&fs);

    free_clusters = update_free(&fs);
    file_unused();

    clean_dirty_flag(&fs);

    if (verbose) {
        print_mem();
#ifdef DEBUG
        print_changes();
#endif
    }
    qfree(&mem_queue);

    if (verify) {
        printf("\nStarting verification pass.\n");
        n_files = 0;
        read_fat(&fs);
        scan_root(&fs);
        check_volume_label(&fs);
        reclaim_free(&fs);
        clean_dirty_flag(&fs);
        if (verbose)
            print_mem();

        qfree(&mem_queue);
    }

    if (fs_changed()) {
        if (rw) {
            if (interactive)
                rw = get_key("yn", "Perform changes ? (y/n)") == 'y';
            else
                printf("Performing changes.\n");
        }
        else
            printf("Leaving file system unchanged.\n");
    }

    printf("%s: %u files, %u/%u clusters\n", argv[optind], n_files,
            fs.clusters - free_clusters, fs.clusters);

    clean_boot(&fs);
    if (fs.fat_cache.addr) {
        fs_munmap(fs.fat_cache.addr, FAT_CACHE_SIZE);
    }

    return fs_close(rw) ? 1 : 0;
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
