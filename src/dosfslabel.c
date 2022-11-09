/* SPDX-License-Identifier : GPL-2.0 */

/* dosfslabel.c  -  User interface */

/* Copyright 2007 Red Hat, Inc.
 * Portions copyright 1998 Roman Hodek.
 * Portions copyright 1993 Werner Almesberger.
 */

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
int atari_format = 0;
unsigned n_files = 0;
void *mem_queue = NULL;

/* TODO: separate code for not compiling uncessary file */
int remain_dirty = 0;   /* Not used : for removing compile error */
uint32_t max_clus_num;  /* Not used : for removing compile error */

static label_t *label_head;
static label_t *label_last;

static void usage(int error)
{
    FILE *f = error ? stderr : stdout;
    int status = error ? 1 : 0;

    fprintf(f, "usage: dosfslabel device [label]\n");
    exit(status);
}

int main(int argc, char *argv[])
{
    DOS_FS fs;
    int rw = 0;
    int save_nfats = 0;
    int ret = 0;

    char *device = NULL;
    char *label = NULL;
    char vol_label[LEN_VOLUME_LABEL + 1] = {'\0', };

    check_atari(&atari_format);

    if (argc < 2 || argc > 3)
        usage(1);

    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))
        usage(0);
    else if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
        printf("dosfslabel " VERSION ", " VERSION_DATE ", FAT32, LFN\n");
        exit(0);
    }

    device = argv[1];
    if (argc == 3) {
        label = argv[2];
        if (strlen(label) > 11) {
            fprintf(stderr,
                    "dosfslabel: labels can be no longer than 11 characters\n");
            exit(1);
        }
        rw = 1;
    }

    fs_open(device, rw);
    read_boot(&fs);

    /* dosfslabel doesn't need second FAT handling in read_fat()
     * so change fs.nfats to 1 temporarily and
     * after calling read_fat() restore original value */
    save_nfats = fs.nfats;
    fs.nfats = 1;
    read_fat(&fs);
    fs.nfats = save_nfats;

    if (!rw) {
        fprintf(stdout, "%s\n", fs.label);

#ifdef DEBUG_TEST
        for (int i = 0; i < strlen(fs.label); i++) {
            fprintf(stdout, "%02x ", fs.label[i]);
        }
        printf("\n");
#endif

        exit(0);
    }

    scan_root_only(&fs, &label_head, &label_last);
    if (label_head && (label_head != label_last)) {
        printf("Multiple label entries in root, do dosfsck first\n");
        goto out;
    }

    memcpy(vol_label, label, strlen(label));
    if (check_valid_label(vol_label) == 0) {
        write_label(&fs, vol_label, &label_head, &label_last);
    }

out:
    clean_boot(&fs);

    ret = fs_flush(rw);
    fs_close();
    return (ret ? EXIT_FAILURE : EXIT_SUCCESS);
}
