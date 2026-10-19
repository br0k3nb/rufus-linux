/*
 * Rufus Linux port: Win32 compatibility shim.
 *
 * Provides the subset of Windows types and APIs used by the vendored
 * libraries (ms-sys, libfat, libinstaller, ext2fs, bled, ...) and by the
 * adapted upstream modules under linux/src, implemented on top of POSIX.
 *
 * Only compiled for the Linux port; upstream (Windows) sources are not
 * affected.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFUS_LINUX_WIN32COMPAT_H
#define RUFUS_LINUX_WIN32COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Base types (Microsoft data model, C99 fixed-width equivalents)      */
/* ------------------------------------------------------------------ */
typedef void            VOID;
typedef int             BOOL;
typedef uint8_t         BOOLEAN;
typedef uint8_t         BYTE;
typedef uint8_t         UINT8;
typedef int8_t          INT8;
typedef uint16_t        WORD;
typedef uint16_t        USHORT;
typedef uint16_t        UINT16;
typedef int16_t         SHORT;
typedef int16_t         INT16;
typedef uint32_t        DWORD;
typedef uint32_t        ULONG;
typedef uint32_t        UINT;
typedef uint32_t        UINT32;
typedef uint32_t        DWORD32;
typedef int32_t         LONG;
typedef int32_t         INT;
typedef int32_t         INT32;
typedef uint64_t        QWORD;
typedef uint64_t        ULONGLONG;
typedef uint64_t        UINT64;
typedef uint64_t        DWORDLONG;
typedef uint64_t        DWORD64;
typedef int64_t         LONGLONG;
typedef int64_t         INT64;
typedef int64_t         LONG64;
typedef uint64_t        ULONG64;
typedef int64_t         LONG_PTR_SPARE_;
typedef uint64_t        ULONGLONG_SPARE_;
typedef uintptr_t       ULONG_PTR;
typedef uintptr_t       UINT_PTR;
typedef uintptr_t       DWORD_PTR;
typedef intptr_t        LONG_PTR;
typedef intptr_t        INT_PTR;
typedef int32_t         HRESULT;
typedef int32_t         SCODE;
typedef int32_t         NTSTATUS;
typedef char            CHAR;
typedef unsigned char   UCHAR;
typedef UCHAR*          PUCHAR;
typedef wchar_t         WCHAR;
typedef char            TCHAR;
typedef ssize_t         SSIZE_T;
typedef size_t          SIZE_T;
typedef ssize_t         LONG64_SPARE_;

typedef LONGLONG*       PLONGLONG;
typedef ULONGLONG*      PULONGLONG;
typedef LONG*           PLONG;
typedef DWORD*          LPDWORD;
typedef LONG*           LPLONG;
typedef WORD*           LPWORD;
typedef BYTE*           LPBYTE;
typedef INT*            LPINT;
typedef UINT*           LPUINT;
typedef BOOL*           LPBOOL;
typedef ULONG*          PULONG;
typedef uint64_t*       PUINT64;
typedef void*           PVOID;
typedef const void*     LPCVOID;
typedef void*           LPVOID;
typedef char*           LPSTR;
typedef const char*     LPCSTR;
typedef wchar_t*        LPWSTR;
typedef const wchar_t*  LPCWSTR;
typedef CHAR*           PCHAR;
typedef void*           HANDLE;
typedef void**          PHANDLE;
typedef uint32_t        HKEY;
typedef void*           HMODULE;
typedef void*           HINSTANCE;
typedef void*           HWND;
typedef void*           HDC;
typedef void*           HFONT;
typedef void*           HMENU;
typedef void*           HRSRC;
typedef void*           HGLOBAL;
typedef void*           HBITMAP;
typedef void*           HCURSOR;
typedef void*           HICON;
typedef void*           LPSECURITY_ATTRIBUTES;
typedef void*           SECURITY_ATTRIBUTES;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif

#define MAX_PATH            260
#define MAXDWORD            0xFFFFFFFFUL
#define MAXUINT32           0xFFFFFFFFUL

/* Calling conventions and annotation no-ops */
#define WINAPI
#define CALLBACK
#define APIENTRY
#define NTAPI
#define __stdcall
#define __cdecl
#define STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#define DECLSPEC_IMPORT
#define __declspec(x)
#define __forceinline       inline __attribute__((always_inline))
#define __inline            inline
#define __unaligned
#define CONST               const
#define VOID_SPARE_         void

#define _countof(x)         (sizeof(x)/sizeof((x)[0]))
#ifndef ARRAYSIZE
#define ARRAYSIZE(x)        (sizeof(x)/sizeof((x)[0]))
#endif

#define IN
#define OUT
#define OPTIONAL
#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#define _Inout_opt_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _In_reads_opt_(x)
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_bytes_to_(a,b)
#define _Outptr_opt_
#define _Ret_maybenull_
#define _Success_(x)
#define _Printf_format_string_
#define _Use_decl_annotations_
#define __RPC__in
#define __RPC__out
#define __RPC__in_opt
#define __RPC__deref_out_opt
#define __RPC__out_ecount_full(x)
#define __RPC__deref_out_ecount_full_opt(x)
#define __RPC__in_ecount_full(x)
#define __RPC__in_ecount_full_opt(x)
#define __RPC__deref_out_ecount_full_opt_string(x)
#define __RPC__in_opt_string
#define __RPC__out_ecount_part(a,b)
#define _COM_Outptr_
#define CONST_VTBL
/* NB: Windows' "interface" keyword (COM) is deliberately not defined: on
 * Linux it collides with structure members named "interface" (e.g. in
 * <linux/usbdevice_fs.h>) */

/* ------------------------------------------------------------------ */
/* Windows error codes used by the ported code                         */
/* ------------------------------------------------------------------ */
#define ERROR_SEVERITY_ERROR        0xC0000000UL
#define APPLICATION_ERROR_MASK      0x20000000UL
#define FACILITY_STORAGE            0x3
#define FAC(f)                      ((ULONG)(f)<<16)

#define ERROR_SUCCESS               0
#define ERROR_INVALID_FUNCTION      1
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_DATA          13
#define ERROR_WRITE_PROTECT         19
#define ERROR_NOT_READY             21
#define ERROR_CRC                   23
#define ERROR_SEEK                  25
#define ERROR_SECTOR_NOT_FOUND      27
#define ERROR_WRITE_FAULT           29
#define ERROR_READ_FAULT            30
#define ERROR_GEN_FAILURE           31
#define ERROR_SHARING_VIOLATION     32
#define ERROR_LOCK_VIOLATION        33
#define ERROR_HANDLE_EOF            38
#define ERROR_HANDLE_DISK_FULL      39
#define ERROR_NOT_SUPPORTED         50
#define ERROR_REM_NOT_LIST          51
#define ERROR_DUP_NAME              52
#define ERROR_BAD_NETPATH           53
#define ERROR_FILE_EXISTS           80
#define ERROR_CANNOT_MAKE           82
#define ERROR_INVALID_PARAMETER     87
#define ERROR_BROKEN_PIPE           109
#define ERROR_OPEN_FAILED           110
#define ERROR_DISK_FULL             112
#define ERROR_CALL_NOT_IMPLEMENTED  120
#define ERROR_INSUFFICIENT_BUFFER   122
#define ERROR_INVALID_NAME          123
#define ERROR_BUSY                  170
#define ERROR_ALREADY_EXISTS        183
#define ERROR_MORE_DATA             234
#define ERROR_OBJECT_IN_LIST        5010
#define ERROR_INVALID_BLOCK         9
#define ERROR_NO_MORE_ITEMS         259
#define ERROR_WRITE_ACCESS_DENIED   1536
#define ERROR_NOT_FOUND             1168
#define ERROR_DEVICE_IN_USE         2404
#define ERROR_LABEL_TOO_LONG        154
#define ERROR_NO_MEDIA_IN_DRIVE     1112
#define ERROR_INSTALL_FAILURE       1603
#define ERROR_PARTITION_FAILURE     1105
#define ERROR_CANNOT_COPY           266
#define ERROR_CANCELLED             1223
#define ERROR_NOT_ENOUGH_QUOTA      1816
#define ERROR_NOT_A_REPARSE_POINT   4390
#define ERROR_IO_PENDING            997
#define ERROR_IO_INCOMPLETE         996
#define ERROR_TIMEOUT               1460
#define ERROR_OFFSET_ALIGNMENT_VIOLATION 327
#define WAIT_OBJECT_0               0
#define WAIT_TIMEOUT                258
#define WAIT_FAILED                 0xFFFFFFFFUL
#define WAIT_ABANDONED              128
#define STATUS_TIMEOUT              0x00000102L
#define INFINITE                    0xFFFFFFFFUL

/* HRESULT-style helpers (upstream rufus.h:1030) */
#define IS_ERROR(hr)                (((HRESULT)(hr)) < 0)
#define SCODE_CODE(sc)              ((ULONG)(sc) & 0xFFFFUL)
#define SCODE_FACILITY(sc)          ((((ULONG)(sc)) >> 16) & 0x1FFFUL)
#define SCODE_SEVERITY(sc)          ((((ULONG)(sc)) >> 30) & 0x3UL)
#define SUCCEEDED(hr)               (((HRESULT)(hr)) >= 0)
#define FAILED(hr)                  (((HRESULT)(hr)) < 0)
#define S_OK                        0
#define S_FALSE                     1
#define E_NOTIMPL                   ((HRESULT)0x80004001L)
#define E_FAIL                      ((HRESULT)0x80004005L)
#define E_OUTOFMEMORY               ((HRESULT)0x8007000EL)
#define E_INVALIDARG                ((HRESULT)0x80070057L)
#define RUFUS_ERROR(err)            (ERROR_SEVERITY_ERROR | FAC(FACILITY_STORAGE) | (err))
#define APPERR(err)                 (APPLICATION_ERROR_MASK | (err))
#define ERROR_INCOMPATIBLE_FS       0x1201
#define ERROR_CANT_QUICK_FORMAT     0x1202
#define ERROR_INVALID_CLUSTER_SIZE  0x1203
#define ERROR_INVALID_VOLUME_SIZE   0x1204
#define ERROR_CANT_START_THREAD     0x1205
#define ERROR_BADBLOCKS_FAILURE     0x1206
#define ERROR_ISO_SCAN              0x1207
#define ERROR_ISO_EXTRACT           0x1208
#define ERROR_CANT_REMOUNT_VOLUME   0x1209
#define ERROR_CANT_PATCH            0x120A
#define ERROR_CANT_ASSIGN_LETTER    0x120B
#define ERROR_CANT_MOUNT_VOLUME     0x120C
#define ERROR_BAD_SIGNATURE         0x120D
#define ERROR_CANT_DOWNLOAD         0x120E
/* Windows winerror.h codes the ported code relies on */
#define ERROR_NO_VOLUME_ID          1173L
#define ERROR_BAD_ARGUMENTS         160L

/* ------------------------------------------------------------------ */
/* Files, handles, basic I/O                                           */
/* ------------------------------------------------------------------ */
#define INVALID_HANDLE_VALUE        ((HANDLE)(intptr_t)-1)
#define INVALID_SET_FILE_POINTER    0xFFFFFFFFUL

#define GENERIC_READ                0x80000000UL
#define GENERIC_WRITE               0x40000000UL
#define GENERIC_ALL                 0x10000000UL
#define FILE_SHARE_READ             0x00000001UL
#define FILE_SHARE_WRITE            0x00000002UL
#define FILE_SHARE_DELETE           0x00000004UL
#define CREATE_NEW                  1
#define CREATE_ALWAYS               2
#define OPEN_EXISTING               3
#define OPEN_ALWAYS                 4
#define TRUNCATE_EXISTING           5
#define FILE_BEGIN                  0
#define FILE_CURRENT                1
#define FILE_END                    2

#define FILE_ATTRIBUTE_READONLY     0x00000001UL
#define FILE_ATTRIBUTE_HIDDEN       0x00000002UL
#define FILE_ATTRIBUTE_SYSTEM       0x00000004UL
#define FILE_ATTRIBUTE_DIRECTORY    0x00000010UL
#define FILE_ATTRIBUTE_ARCHIVE      0x00000020UL
#define FILE_ATTRIBUTE_NORMAL       0x00000080UL
#define FILE_ATTRIBUTE_TEMPORARY    0x00000100UL
#define FILE_ATTRIBUTE_SPARSE_FILE  0x00000200UL
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400UL
#define FILE_ATTRIBUTE_UNCOMPRESSED 0x00008000UL
#define FILE_FLAG_WRITE_THROUGH     0x80000000UL
#define FILE_FLAG_OVERLAPPED        0x40000000UL
#define FILE_FLAG_NO_BUFFERING      0x20000000UL
#define FILE_FLAG_SEQUENTIAL_SCAN   0x08000000UL
#define FILE_FLAG_BACKUP_SEMANTICS  0x02000000UL
#define FILE_FLAG_DELETE_ON_CLOSE   0x04000000UL
#define FILE_TYPE_UNKNOWN           0x0000
#define FILE_TYPE_DISK              0x0001

#define INVALID_FILE_ATTRIBUTES     0xFFFFFFFFUL
#define INVALID_FILE_SIZE           0xFFFFFFFFUL

typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t  HighPart;
    };
    struct {
        uint32_t LowPart;
        int32_t  HighPart;
    } u;
    int64_t  QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        uint32_t LowPart;
        uint32_t HighPart;
    };
    struct {
        uint32_t LowPart;
        uint32_t HighPart;
    } u;
    uint64_t QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID, *LPGUID, *PGUID;

typedef struct _WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    CHAR     cFileName[MAX_PATH];
    CHAR     cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA, WIN32_FIND_DATA, *LPWIN32_FIND_DATA;

/* Internal handle representation: pointer to this struct (see io.c) */
struct posix_handle;

typedef struct OVERLAPPED_COMPAT {
    ULONG_PTR Internal[2];
    ULONG64   Offset;
    HANDLE    hEvent;
    BOOL      bOffsetUpdated;
} OVERLAPPED, *LPOVERLAPPED;

/* ---- Core file API (implemented in linux/src/io.c) ---- */
HANDLE  CreateFileA(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE  CreateFileU(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL    ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                 LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL    WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                  LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL    WriteFileWithRetry(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                           LPDWORD lpNumberOfBytesWritten, DWORD nRetries);
BOOL    SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                         PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
DWORD   SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh,
                       DWORD dwMoveMethod);
BOOL    GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
DWORD   GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL    SetEndOfFile(HANDLE hFile);
BOOL    FlushFileBuffers(HANDLE hFile);
BOOL    CloseHandle(HANDLE hObject);
DWORD   GetFileType(HANDLE hFile);

BOOL    DeleteFileA(const char* lpFileName);
BOOL    DeleteFileU(const char* lpFileName);
BOOL    MoveFileA(const char* lpExistingFileName, const char* lpNewFileName);
BOOL    MoveFileU(const char* lpExistingFileName, const char* lpNewFileName);
BOOL    MoveFileExU(const char* lpExistingFileName, const char* lpNewFileName, DWORD dwFlags);
BOOL    CopyFileA(const char* lpExistingFileName, const char* lpNewFileName, BOOL bFailIfExists);
BOOL    CopyFileU(const char* lpExistingFileName, const char* lpNewFileName, BOOL bFailIfExists);
BOOL    CreateDirectoryA(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL    CreateDirectoryU(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
int     SHCreateDirectoryExU(HWND hwnd, const char* pszPath, const void* psa);
typedef unsigned short FILEOP_FLAGS;
#define FOF_NO_UI               0x0614
int     SHDeleteDirectoryExU(HWND hwnd, const char* pszPath, FILEOP_FLAGS fFlags);
#define NO_ERROR                0L
#define fopenU(f, m)            fopen(f, m)
BOOL    RemoveDirectoryA(const char* lpPathName);
DWORD   GetFileAttributesA(const char* lpFileName);
DWORD   GetFileAttributesU(const char* lpFileName);
BOOL    SetFileAttributesU(const char* lpFileName, DWORD dwFileAttributes);
BOOL    PathFileExistsU(const char* szPath);
DWORD   GetFullPathNameU(const char* lpFileName, DWORD nBufferLength, char* lpBuffer, char** lpFilePart);
BOOL    CreateSymbolicLinkU(const char* lpSymlinkFileName, const char* lpTargetFileName, DWORD dwFlags);
BOOL    GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
BOOL    SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime);
DWORD   GetTempPathU(DWORD nBufferLength, char* lpBuffer);
DWORD   GetTempFileNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempFileName);
DWORD   GetTempDirNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempDirName);
DWORD   GetModuleFileNameA(HMODULE hModule, char* lpFilename, DWORD nSize);
DWORD   GetModuleFileNameU(HMODULE hModule, char* lpFilename, DWORD nSize);

/* Port helper: realpath() into a malloc'd buffer (caller frees) */
char*   realpath_dup(const char* path);
DWORD   GetCurrentDirectoryU(DWORD nBufferLength, char* lpBuffer);
BOOL    SetCurrentDirectoryU(const char* lpPathName);
int     _chdirU(const char* dirname);
int     _mkdirU(const char* dirname);
int     _rmdirU(const char* dirname);
int     _unlinkU(const char* path);
int     _accessU(const char* path, int mode);
int     _stat64U(const char* path, struct stat* buf);
uint64_t _filesizeU(const char* path);
int     _openU(const char* filename, int oflag, int pmode);
HANDLE  FindFirstFileU(const char* lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL    FindNextFileU(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL    FindClose(HANDLE hFindFile);
BOOL    PathIsDirectoryU(const char* pszPath);

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */
DWORD   GetLastError(void);
void    SetLastError(DWORD dwErrCode);
const char* WindowsErrorString(void);
const char* StrError(DWORD error_code, BOOL use_default_locale);

/* ------------------------------------------------------------------ */
/* Threads, events, synchronisation                                    */
/* ------------------------------------------------------------------ */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
HANDLE  CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                     LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                     DWORD dwCreationFlags, LPDWORD lpThreadId);
void    ExitThread(DWORD dwExitCode);
DWORD   WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD   WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
BOOL    SetEvent(HANDLE hEvent);
BOOL    ResetEvent(HANDLE hEvent);
HANDLE  CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                     BOOL bInitialState, const char* lpName);
HANDLE  CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                     BOOL bInitialState, const wchar_t* lpName);
HANDLE  CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, const char* lpName);
BOOL    ReleaseMutex(HANDLE hMutex);
void    Sleep(DWORD dwMilliseconds);
DWORD   GetTickCount(void);
uint64_t GetTickCount64(void);
ULONGLONG GetTickCount64U(void);
DWORD   GetCurrentThreadId(void);
DWORD   GetCurrentProcessId(void);
BOOL    SetThreadPriority(HANDLE hThread, int nPriority);
DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
BOOL    TerminateThread(HANDLE hThread, DWORD dwExitCode);
int     GetThreadPriority(HANDLE hThread);
#define THREAD_PRIORITY_LOWEST          (-2)
#define THREAD_PRIORITY_BELOW_NORMAL    (-1)
#define THREAD_PRIORITY_NORMAL          0
#define THREAD_PRIORITY_ABOVE_NORMAL    1
#define THREAD_PRIORITY_HIGHEST         2
#define THREAD_PRIORITY_BELOW_NORMAL_SPARE 0

LONG    InterlockedIncrement(LONG volatile* Addend);
LONG    InterlockedDecrement(LONG volatile* Addend);
LONG    InterlockedExchange(LONG volatile* Target, LONG Value);
LONG64  InterlockedExchange64(LONG64 volatile* Target, LONG64 Value);
PVOID   InterlockedExchangePointer(PVOID volatile* Target, PVOID Value);
LONG    InterlockedCompareExchange(LONG volatile* Destination, LONG Exchange, LONG Comparand);
LONG64  InterlockedCompareExchange64(LONG64 volatile* Destination, LONG64 Exchange, LONG64 Comparand);

/* Timer shim, matching the Rufus SetTimer(hWnd, id, ms, callback) usage */
typedef void (CALLBACK *TIMERPROC)(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
#ifndef WM_TIMER
#define WM_TIMER                0x0113
#endif
UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);
BOOL     KillTimer(HWND hWnd, UINT_PTR uIDEvent);

/* Critical sections on pthread mutexes */
typedef pthread_mutex_t CRITICAL_SECTION;
void    InitializeCriticalSection(CRITICAL_SECTION* cs);
void    DeleteCriticalSection(CRITICAL_SECTION* cs);
void    EnterCriticalSection(CRITICAL_SECTION* cs);
void    LeaveCriticalSection(CRITICAL_SECTION* cs);

/* ------------------------------------------------------------------ */
/* CPU / intrinsics                                                    */
/* ------------------------------------------------------------------ */
static __inline void __cpuid(int regs[4], int function_id)
{
    __asm__ __volatile__("cpuid"
        : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
        : "a"(function_id));
}
static __inline void __cpuidex(int regs[4], int function_id, int subfunction_id)
{
    __asm__ __volatile__("cpuid"
        : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
        : "a"(function_id), "c"(subfunction_id));
}
/* Aligned allocation (MSVC's <mm_malloc.h>). GCC ships the same header for
 * x86 (which is exactly what the vendored ms-sys code expects); fall back to
 * a POSIX implementation elsewhere. */
#if defined(__i386__) || defined(__x86_64__)
#include <mm_malloc.h>
#else
static __inline void* _mm_malloc(size_t size, size_t alignment)
{
    void* ptr = NULL;
    if (alignment < sizeof(void*))
        alignment = sizeof(void*);
    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

static __inline void _mm_free(void* ptr)
{
    free(ptr);
}
#endif

/* ------------------------------------------------------------------ */
/* MSVC-isms / string helpers                                          */
/* ------------------------------------------------------------------ */
#define _stricmp                strcasecmp
#define _strnicmp               strncasecmp
#define stricmp                 strcasecmp
#define strnicmp                strncasecmp
#define _strdup                 strdup
#define _wcsdup                 wcsdup
#define _snprintf               snprintf
#define _vsnprintf              vsnprintf
#define _snwprintf              swprintf
#define _alloca                 alloca
#define _rotl(x, n)             (((x) << ((n) & 31)) | ((x) >> (32 - ((n) & 31))))
#define _rotr(x, n)             (((x) >> ((n) & 31)) | ((x) << (32 - ((n) & 31))))
#define _byteswap_ushort        __builtin_bswap16
#define _byteswap_ulong         __builtin_bswap32
#define _byteswap_uint64        __builtin_bswap64
#define _itoa64(u, s, r)
#define __debugbreak()          __builtin_trap()
#define _CRT_ALIGN(n)           __attribute__((aligned(n)))

#define sprintf_s               snprintf
/* MSVC's _snprintf_s(buf, size, count, fmt, ...): count is _TRUNCATE in Rufus */
#define _TRUNCATE               ((size_t)-1)
#define _snprintf_s(buf, size, count, ...)  snprintf(buf, size, __VA_ARGS__)
#define vsprintf_s              vsnprintf
#define _vsnprintf_s            vsnprintf
#define strcpy_s(d, sz, s)      do { strncpy((d), (s), (sz)); (d)[(sz)-1] = 0; } while(0)
#define strcat_s(d, sz, s)      do { size_t _l = strlen(d); strncpy((d)+_l, (s), (sz)-_l); (d)[(sz)-1] = 0; } while(0)
#define memcpy_s(d, sz, s, n)   do { size_t _n = (n); if (_n > (sz)) _n = (sz); memcpy((d), (s), _n); } while(0)
#define strncat_s
#define _snwprintf_s

#define TEXT(x)                 x
#define _T(x)                   x
#define MAKEWORD(a, b)          ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#define MAKELONG(a, b)          ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))
#define LOBYTE(w)               ((BYTE)((w) & 0xFF))
#define HIBYTE(w)               ((BYTE)(((w) >> 8) & 0xFF))
#define LOWORD(l)               ((WORD)((l) & 0xFFFF))
#define HIWORD(l)               ((WORD)(((l) >> 16) & 0xFFFF))
#define LODWORD(l)              ((DWORD)(l))
#define HIDWORD(l)              ((DWORD)(((uint64_t)(l)) >> 32))
#define MAKELONGLONG(a, b)      ((LONGLONG)(((DWORD)(a)) | (((LONGLONG)((DWORD)(b))) << 32)))

#define ZeroMemory(d, n)        memset((d), 0, (n))
#define CopyMemory(d, s, n)     memmove((d), (s), (n))
#define MoveMemory(d, s, n)     memmove((d), (s), (n))
#define FillMemory(d, n, v)     memset((d), (v), (n))

/* min/max may be used by vendored code */
#ifndef min
#define min(a, b)               (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b)               (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))
#endif

/* Linux port handle helpers (io.c) */
void        SetHandleOffset(HANDLE hFile, uint64_t offset);
uint64_t    GetHandleOffset(HANDLE hFile);
const char* GetHandlePath(HANDLE hFile);
uint64_t    GetHandleRawSize(HANDLE hFile);
int         GetHandleFD(HANDLE hFile);

/* FILE* helpers used by vendored code on Windows */
typedef FILE POSIX_FILE_ALIAS;

/* ------------------------------------------------------------------ */
/* Misc constants used by shared code                                  */
/* ------------------------------------------------------------------ */
#define INVALID_FILE_SIZE_           0xFFFFFFFFUL
#define FILE_NAME_NORMALIZED         0
#define FILE_NAME_OPENED             0x8
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#define MOVEFILE_REPLACE_EXISTING    0x1
#define MOVEFILE_COPY_ALLOWED        0x2

#define MEM_COMMIT                   0x1000
#define MEM_RESERVE                  0x2000
#define MEM_RELEASE                  0x8000
#define PAGE_READWRITE               0x04
#define PAGE_NOACCESS                0x01

#define STILL_ACTIVE                 259
#define THREAD_PRIORITY_SPARE_       0

/* Format message helpers (used with FormatMessageU in upstream code) */
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE    0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_ARGUMENT_ARRAY  0x00002000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK  0x000000FF

/* MessageBox flags/results (used by Notification() call sites) */
#define MB_OK                          0x00000000
#define MB_OKCANCEL                    0x00000001
#define MB_ABORTRETRYIGNORE            0x00000002
#define MB_YESNOCANCEL                 0x00000003
#define MB_YESNO                       0x00000004
#define MB_RETRYCANCEL                 0x00000005
#define MB_CANCELTRYCONTINUE           0x00000006
#define MB_ICONHAND                    0x00000010
#define MB_ICONQUESTION                0x00000020
#define MB_ICONEXCLAMATION             0x00000030
#define MB_ICONASTERISK                0x00000040
#define MB_ICONERROR                   MB_ICONHAND
#define MB_ICONWARNING                 MB_ICONEXCLAMATION
#define MB_ICONINFORMATION             MB_ICONASTERISK
#define MB_DEFBUTTON1                  0x00000000
#define MB_DEFBUTTON2                  0x00000100
#define MB_DEFBUTTON3                  0x00000200
#define MB_APPLMODAL                   0x00000000
#define MB_SYSTEMMODAL                 0x00001000
#define MB_SETFOREGROUND               0x00010000
#define IDOK                           1
#define IDCANCEL                       2
#define IDABORT                        3
#define IDRETRY                        4
#define IDIGNORE                       5
#define IDYES                          6
#define IDNO                           7
#define IDCLOSE                        8
#define IDHELP                         9
#define IDTRYAGAIN                     10
#define IDCONTINUE                     11

/* Resource access (upstream code uses MAKEINTRESOURCEA with IDR_* IDs;
 * the Linux port maps those IDs to files under res/ - see resources.c) */
#define MAKEINTRESOURCEA(i)            ((char*)(uintptr_t)((WORD)(i)))
#define MAKEINTRESOURCEW(i)            ((wchar_t*)(uintptr_t)((WORD)(i)))
#ifndef MAKEINTRESOURCE
#define MAKEINTRESOURCE                MAKEINTRESOURCEA
#endif
#ifndef _RT_RCDATA
#define _RT_RCDATA                     MAKEINTRESOURCEA(10)
#endif
#define RT_RCDATA                      MAKEINTRESOURCEA(10)
#ifndef _RT_GROUP_ICON
#define _RT_GROUP_ICON		MAKEINTRESOURCEA((ULONG_PTR)(MAKEINTRESOURCEA(3) + 11))
#endif
#define RT_GROUP_ICON                  MAKEINTRESOURCEA(14)

/* Resources are looked up by numeric ID in the port (resources.c) */
uint8_t* GetResource(HMODULE module, char* name, char* type, const char* desc, DWORD* len, BOOL duplicate);
DWORD    GetResourceSize(HMODULE module, char* name, char* type, const char* desc);
void     free_resource_cache(void);

/* PE/COFF structures */
#include "winnt_pe.h"

/* Misc. Win32 APIs used by ported code */
#define CreateEvent             CreateEventA
#define GetCurrentThread()      ((HANDLE)NULL)  /* SetThreadAffinityMask() treats NULL as 'self' */
#define lstrcmpiA(a, b)         strcasecmp(a, b)
#define PathFileExistsA(p)      PathFileExistsU(p)
static __inline char* StrStrIA(const char* hay, const char* needle) { return strcasestr(hay, needle); }

/* MSVCRT low level I/O names */
#define _O_RDONLY               O_RDONLY
#define _O_WRONLY               O_WRONLY
#define _O_RDWR                 O_RDWR
#define _O_CREAT                O_CREAT
#define _O_TRUNC                O_TRUNC
#define _O_EXCL                 O_EXCL
#define _O_APPEND               O_APPEND
#define _O_BINARY               0
#define _S_IREAD                S_IRUSR
#define _S_IWRITE               (S_IWUSR | S_IRGRP | S_IROTH)
#define _SH_DENYNO              0
#define _close                  close
#define _read(fd, buf, n)       ((int)read(fd, buf, n))
#define _write(fd, buf, n)      ((int)write(fd, buf, n))
#define _chmod                  chmod
#define _unlink                 unlink
static __inline int _sopen_s(int* pfh, const char* filename, int oflag, int shflag, int pmode)
{
    (void)shflag;
    *pfh = open(filename, oflag | O_CLOEXEC, pmode);
    return (*pfh < 0) ? errno : 0;
}
/* Get a POSIX file descriptor for a HANDLE, positioned at the handle's
 * current offset. The descriptor must be closed with _close(). */
int     _open_osfhandle(intptr_t osfhandle, int flags);

#endif /* RUFUS_LINUX_WIN32COMPAT_H */
