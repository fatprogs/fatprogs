/* common.h  -  Common functions */

/* Written 1993 by Werner Almesberger */

# include <asm/types.h>
# define MSDOS_FAT12 4084 /* maximum number of clusters in a 12 bit FAT */

#ifndef _COMMON_H
#define _COMMON_H

#ifndef offsetof
#define offsetof(t, e)	((off_t)&(((t *)0)->e))
#endif

/* don't divide by zero */
#define ROUND_TO_MULTIPLE(n, m) \
    ((n) && (m) ? (n) + (m) - 1 - ((n) - 1) % (m) : 0)

/* Displays a prinf-style message and terminates the program. */
void die(char *msg, ...) __attribute((noreturn));

/* Like die, but appends an error message according to the state of errno. */
void pdie(char *msg, ...) __attribute((noreturn));

/* mallocs SIZE bytes and returns a pointer to the data.
 * Terminates the program if malloc fails. */
void *alloc(int size);

/* Like alloc, but registers the data area in a list described by ROOT. */
void *qalloc(void **root, int size);

/* Deallocates all qalloc'ed data areas described by ROOT. */
void qfree(void **root);

/* Returns the smaller integer value of a and b. */
int min(int a, int b);

/* Displays PROMPT and waits for user input. Only characters in VALID are
   accepted. Terminates the program on EOF. Returns the character. */
char get_key(char *valid, char *prompt);

void check_atari(int *atari_format);

#endif
