/* SPDX-License-Identifier : GPL-2.0 */

/* common.h  -  Common functions */

/* Written 1993 by Werner Almesberger */

#include <asm/types.h>
#define MSDOS_FAT12 4084 /* maximum number of clusters in a 12 bit FAT */

#ifndef _COMMON_H
#define _COMMON_H

#ifndef offsetof
#define offsetof(t, e)	((off_t)&(((t *)0)->e))
#endif

/* don't divide by zero */
#define ROUND_TO_MULTIPLE(n, m) \
    ((n) && (m) ? (n) + (m) - 1 - ((n) - 1) % (m) : 0)

#define BITS_PER_BYTE   8
#define BITS_PER_LONG   (sizeof(long) * BITS_PER_BYTE)
#define BITS_PER_LONG_LONG  (sizeof(long long) * BITS_PER_BYTE)

/* macro from linux kernel include/linux/bits.h */
#define BIT(nr)         ((1UL) << (nr))
#define BIT_ULL(nr)     ((1ULL) << (nr))
#define BIT_MASK(nr)        ((1UL) << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)        ((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)    ((1ULL) << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)    ((nr) / BITS_PER_LONG_LONG)
/**/

#define DEFAULT_FAT_BUF     (96 * 1024) /* Common Multiple of FAT12,16,32 */

/* Displays a prinf-style message and terminates the program. */
void die(char *msg, ...) __attribute((noreturn));

/* Like die, but appends an error message according to the state of errno. */
void pdie(char *msg, ...) __attribute((noreturn));

/* mallocs SIZE bytes and returns a pointer to the data.
 * Terminates the program if malloc fails. */
void *alloc_mem(int size);
void free_mem(void *p);
void print_mem(void);

/* Like alloc, but registers the data area in a list described by ROOT. */
void *qalloc(void **root, int size);

/* Deallocates all qalloc'ed data areas described by ROOT. */
void qfree(void **root);

/* Returns the smaller integer value of a and b. */
int min(int a, int b);
/* Returns the larger integer value of a and b. */
int max(int a, int b);

/* Displays PROMPT and waits for user input. Only characters in VALID are
   accepted. Terminates the program on EOF. Returns the character. */
char get_key(char *valid, char *prompt);

void check_atari(int *atari_format);

void set_bit(unsigned int nr, unsigned long *p);
void clear_bit(unsigned int nr, unsigned long *p);
int test_bit(unsigned int nr, unsigned long *p);
void change_bit(unsigned int nr, unsigned long *p);

#endif
