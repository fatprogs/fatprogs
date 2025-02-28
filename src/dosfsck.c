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
#include <signal.h>

#include "common.h"
#include "dosfsck.h"
#include "io.h"
#include "boot.h"
#include "fat.h"
#include "file.h"
#include "check.h"

int interactive = 0, list = 0, test = 0, verbose = 0, write_immed = 0;
int check_dirty_only = 0;
int atari_format = 0;
int remain_dirty = 0;
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
}

/* SIGBUS signal handler. It is only useful for mmap without POPULATE and
 * try to access to their mmapped address that device was already gone. */
static void handle_signal(int signum)
{
    pdie("Received SIGBUS signal, exit!!\n");
}

static int setup_signal(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;

    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        fprintf(stderr, "ERR: failed to set signal handler\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    DOS_FS fs;
    int rw, salvage_files, verify, c;
    int ret = 0;
    int dirty_flag = 0;
    uint32_t free_clusters;

    salvage_files = verify = 0;
    rw = 1;
    interactive = 1;
    check_atari(&atari_format);

    setup_signal();

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
                break;
            case 'V':
                verify = 1;
                break;
            case 'w':
                write_immed = 1;
                break;
            default:
                usage(argv[0]);
                exit(EXIT_SYNTAX_ERROR);
        }
    }

    if ((test || write_immed) && !rw) {
        fprintf(stderr, "-t and -w require -a or -r\n");
        exit(EXIT_SYNTAX_ERROR);
    }

    if (optind != argc - 1) {
        usage(argv[0]);
        exit(EXIT_SYNTAX_ERROR);
    }

    printf("dosfsck " VERSION ", " VERSION_DATE ", FAT32, LFN\n");
    fs_open(argv[optind], rw);
    read_boot(&fs);

    if (verify)
        printf("\nStarting check/repair pass.\n");

    do {
        n_files = 0;
        dirty_flag = 0;
        read_fat(&fs);

        if ((fs.fat_bits == 32) || (fs.fat_bits == 16)) {
            dirty_flag = check_dirty_flag(&fs);
        }

        if (check_dirty_only) {
            if (dirty_flag) {
                if (verify)
                    printf("  Just check filesystem dirty flag, exit!\n");
                exit(EXIT_ERRORS_LEFT);
            }
            else {
                if (verify)
                    printf("  Filesystem dirty flag is clean. exit!\n");
                exit(EXIT_NO_ERRORS);
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
        if (verbose)
            print_mem();

        qfree(&mem_queue);
    }

    if (fs_changed()) {
        if (rw) {
            if (interactive)
                rw = get_key("yn", "Perform changes ? (y/n)") == 'y';
            else
                printf("\nPerforming changes.\n");
        }
        else
            printf("\nLeaving file system unchanged.\n");
    }

    printf("%s: %u files, %u/%u clusters\n", argv[optind], n_files,
            fs.clusters - free_clusters, fs.clusters);

    clean_boot(&fs);

    /* sync for modified data */
    ret = fs_flush(rw);

    if (!remain_dirty && rw)
        clean_dirty_flag(&fs);

    if (fs.fat_cache.addr) {
        fs_munmap(fs.fat_cache.addr, FAT_CACHE_SIZE);
    }

    /* sync for dirty flag */
    fs_flush(rw);

    fs_close();
    if (remain_dirty)
        return EXIT_ERRORS_LEFT;

    return (ret ? EXIT_CORRECTED : EXIT_NO_ERRORS);
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
