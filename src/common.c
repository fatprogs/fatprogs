/* SPDX-License-Identifier : GPL-2.0 */

/* common.c  -  Common functions */

/* Written 1993 by Werner Almesberger */

/* FAT32, VFAT, Atari format support, and various fixes additions May 1998
 * by Roman Hodek <Roman.Hodek@informatik.uni-erlangen.de> */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#ifdef __GNUC__
#include <malloc.h>
#endif

#include "common.h"

unsigned long max_alloc = 0;
unsigned long total_alloc = 0;

typedef struct _link {
    void *data;
    struct _link *next;
} LINK;

void die(char *msg,...)
{
    va_list args;

    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(0x08);
}

void pdie(char *msg,...)
{
    va_list args;

    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr,":%s\n", strerror(errno));
    exit(0x08);
}

void *alloc_mem(int size)
{
    void *this;

    if ((this = malloc(size))) {
        memset(this, 0, size);

        total_alloc += size;
        if (total_alloc > max_alloc)
            max_alloc = total_alloc;

        return this;
    }

    pdie("malloc");
    return NULL; /* for GCC */
}

void free_mem(void *p)
{
    if (!p)
        return;

#ifdef __GNUC__
    total_alloc -= malloc_usable_size(p);
#endif
    free(p);
}

void *qalloc(void **root, int size)
{
    LINK *link;

    link = alloc_mem(sizeof(LINK));
    link->next = *root;
    *root = link;
    return link->data = alloc_mem(size);
}

void qfree(void **root)
{
    LINK *this;

    while (*root) {
        this = (LINK *) *root;
        *root = this->next;
        free_mem(this->data);
        free_mem(this);
    }

    max_alloc = 0;
    total_alloc = 0;
}

int min(int a, int b)
{
    return a < b ? a : b;
}

int max(int a, int b)
{
    return a < b ? b : a;
}

char get_key(char *valid, char *prompt)
{
    int ch, okay;

    while (1) {
        if (prompt)
            printf("%s ", prompt);
        fflush(stdout);

        while (ch = getchar(), ch == ' ' || ch == '\t');
        if (ch == EOF)
            exit(0x08);

        if (!strchr(valid, okay = ch))
            okay = 0;

        while (ch = getchar(), ch != '\n' && ch != EOF);
        if (ch == EOF)
            exit(0x08);

        if (okay)
            return okay;

        printf("Invalid input.\n");
    }
}

/*
 * ++roman: On m68k, check if this is an Atari; if yes, turn on Atari variant
 * of MS-DOS filesystem by default.
 */
void check_atari(int *atari_format)
{
#ifdef __mc68000__
    FILE *f;
    char line[128], *p;

    if (!(f = fopen("/proc/hardware", "r"))) {
        perror("/proc/hardware");
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Model:", 6) == 0) {
            p = line + 6;
            p += strspn(p, " \t");
            if (strncmp(p, "Atari ", 6) == 0)
                *atari_format = 1;
            break;
        }
    }
    fclose(f);
#endif
}

/* bit operation function from linux bitops source */
void set_bit(unsigned int nr, unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = addr + BIT_WORD(nr);

    *p |= mask;
}

void clear_bit(unsigned int nr, unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = addr + BIT_WORD(nr);

    *p &= ~mask;

}

void change_bit(unsigned int nr, unsigned long *addr)
{
    unsigned long mask = BIT_MASK(nr);
    unsigned long *p = addr + BIT_WORD(nr);

    *p ^= mask;
}

int test_bit(unsigned int nr, unsigned long *addr)
{
    return 1UL & (addr[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1)));
}

void print_mem(void)
{
    unsigned long hmem;
    unsigned long lmem;

    printf("Total allocated memory is %ld Bytes\n", total_alloc);
    printf("Maximum allocated memory is ");

    lmem = hmem = max_alloc;
    if ((hmem >> 10) == 0) {
        printf("%ld Bytes\n", lmem);
        return;
    }

    hmem >>= 10;
    if ((hmem >> 10) == 0) {
        printf("%ld.%ld KBytes\n", hmem, lmem % (1 << 10));
        return;
    }

    lmem = hmem;
    hmem >>= 10;
    if ((hmem >> 10) == 0) {
        printf("%ld.%ld MBytes\n", hmem, lmem % (1 << 10));
        return;
    }

    lmem = hmem;
    hmem >>= 10;
    if ((hmem >> 10) == 0) {
        printf("%ld.%ld GBytes\n", hmem, lmem % (1 << 10));
        return;
    }

    printf("more than PBytes\n");
}

/* Local Variables: */
/* tab-width: 8     */
/* End:             */
