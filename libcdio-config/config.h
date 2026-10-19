/* config.h for libcdio — Linux (POSIX) configuration.
 *
 * This is the Linux counterpart of the Windows config.h that ships in
 * upstream/src/libcdio. It is picked up first by putting this directory before
 * the libcdio source directory in the include path, and it only enables
 * the POSIX features the vendored libcdio sources actually use.
 */

#define EMPTY_ARRAY_SIZE

#define HAVE_CHDIR 1
#define HAVE_DAYLIGHT 1
#define HAVE_DRAND48 1
#define HAVE_ICONV 1
#define ICONV_CONST
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FSEEKO 1
#define HAVE_FTRUNCATE 1
#define HAVE_LSEEK64 1
#define HAVE_INTTYPES_H 1
#define HAVE_JOLIET 1
#define HAVE_LIMITS_H 1
#define HAVE_LSTAT 1
#define HAVE_MEMCPY 1
#define HAVE_MEMORY_H 1
#define HAVE_MEMSET 1
#define HAVE_RAND 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1
#define HAVE_ROCK 1
#define HAVE_SETENV 1
#define HAVE_SLEEP 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRDUP 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STRNDUP 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_S_ISLNK 1
#define HAVE_S_ISSOCK 1
#define HAVE_TIMEZONE_VAR 1
#define HAVE_TM_GMTOFF 1
#define HAVE_STRUCT_TM_TM_ZONE 1
#define HAVE_TIMEGM 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LANGINFO_CODESET 1
#define HAVE_TZNAME 1
#define HAVE_TZSET 1
#define HAVE_UNISTD_H 1
#define HAVE_UNSETENV 1
#define HAVE_USLEEP 1

/* Not available (Windows-only) */
/* #undef HAVE_WINDOWS_H */
/* #undef HAVE_WIN32_CDROM */
/* #undef HAVE__STATI64 */

/* Define this if you want to use the 2020 version of the libcdio API. */
#define DO_NOT_WANT_COMPATIBILITY 1

/* Is set when libcdio's config.h has been included. */
#define LIBCDIO_CONFIG_H 1

#define STDC_HEADERS 1
#define _FILE_OFFSET_BITS 64
