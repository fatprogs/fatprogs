# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.71])
AC_INIT([fatprogs], [2.14.0], [osc.jsim@gmail.com])
AC_DEFINE([VERSION_DATE], ["2025-03-10"], [Version date])
AC_CONFIG_SRCDIR([src/common.c])

# Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_PROG_CPP

# Automake
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

# Core definitions
AC_DEFINE([_FILE_OFFSET_BITS], [64], [Define to 64 for large file support])
AC_DEFINE([CONFIG_SYNC_FILE_RANGE], [1], [Enable sync_file_range support])

# Checks for libraries.
PKG_CHECK_MODULES([BLKID], [blkid], [
    AC_DEFINE([HAVE_LIBBLKID], [1], [Define to 1 if you have libblkid])
    AC_SUBST([BLKID_CFLAGS])
    AC_SUBST([BLKID_LIBS])
], [
    AC_MSG_WARN([libblkid not found -> foreign FS detection will be disabled in mkdosfs])
])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h malloc.h mntent.h stdint.h sys/ioctl.h sys/mount.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_INT32_T
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_CHECK_MEMBERS([struct stat.st_rdev])
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_GETMNTENT
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_CHECK_FUNCS([alarm getmntent memmove memset strchr strerror strrchr strspn strtol strtoul strtoull])

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT

# Programs
AC_PROG_CC(gcc cc)
AC_PROG_LN_S
AM_PROG_CC_C_O

# Checks for programs
AC_PROG_INSTALL

PKG_PROG_PKG_CONFIG
