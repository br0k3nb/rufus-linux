/* wimlib configuration for the Rufus Linux port (in lieu of autoconf) */
#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_SYSCALL_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_FUTIMENS 1
#define HAVE_UTIMENSAT 1
#define HAVE_STAT_NANOSECOND_PRECISION 1
#define HAVE_LINUX_XATTR_SUPPORT 1
#define STDC_HEADERS 1
#define PACKAGE "wimlib"
#define PACKAGE_BUGREPORT "https://wimlib.net/forums/"
#define PACKAGE_NAME "wimlib"
#define PACKAGE_STRING "wimlib 1.14.4-3-g4a34203c"
#define PACKAGE_TARNAME "wimlib"
#define PACKAGE_URL ""
#define PACKAGE_VERSION "1.14.4-3-g4a34203c"
#define VERSION "1.14.4-3-g4a34203c"
/* Rufus extension: open WIM files located inside ISO images ("iso|path") */
#define WITH_LIBCDIO 1
