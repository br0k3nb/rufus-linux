#define _GNU_SOURCE
/*
 * Rufus Linux port: POSIX implementation of the Win32 I/O subset declared
 * in linux/include/win32compat.h.
 *
 * The HANDLE representation is a pointer to struct posix_handle; handles
 * cover files/block devices, events, threads, mutexes and timers.
 *
 * Copyright © 2026 The Rufus Linux port authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "win32compat.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>

/* ------------------------------------------------------------------ */
/* Globals shared with ported modules                                  */
/* ------------------------------------------------------------------ */
DWORD LastWriteError = 0;
BOOL  large_drive = FALSE;
/* NB: ulBytesPerSector is defined in ms-sys' br.c (the vendored library
 * owns it, exactly as on Windows) */

/* ------------------------------------------------------------------ */
/* Last error (per-thread, like Windows)                               */
/* ------------------------------------------------------------------ */
static __thread DWORD tls_last_error = ERROR_SUCCESS;

DWORD GetLastError(void)
{
    return tls_last_error;
}

void SetLastError(DWORD dwErrCode)
{
    tls_last_error = dwErrCode;
}

const char* WindowsErrorString(void)
{
    static __thread char err_string[256];
    DWORD err = tls_last_error;
    const char* msg = NULL;

    switch (err) {
    case ERROR_SUCCESS: msg = "Success"; break;
    case ERROR_FILE_NOT_FOUND: msg = "The system cannot find the file specified"; break;
    case ERROR_PATH_NOT_FOUND: msg = "The system cannot find the path specified"; break;
    case ERROR_ACCESS_DENIED: msg = "Access is denied"; break;
    case ERROR_WRITE_PROTECT: msg = "The media is write protected"; break;
    case ERROR_CRC: msg = "Data error (cyclic redundancy check)"; break;
    case ERROR_SECTOR_NOT_FOUND: msg = "The drive cannot locate a specific area or track on the disk"; break;
    case ERROR_WRITE_FAULT: msg = "The system cannot write to the specified device"; break;
    case ERROR_READ_FAULT: msg = "The system cannot read from the specified device"; break;
    case ERROR_GEN_FAILURE: msg = "A device attached to the system is not functioning"; break;
    case ERROR_SHARING_VIOLATION: msg = "The process cannot access the file because it is being used by another process"; break;
    case ERROR_HANDLE_EOF: msg = "Reached the end of the file"; break;
    case ERROR_HANDLE_DISK_FULL: msg = "The disk is full"; break;
    case ERROR_NOT_SUPPORTED: msg = "The request is not supported"; break;
    case ERROR_BUSY: msg = "The requested resource is in use"; break;
    case ERROR_DISK_FULL: msg = "There is not enough space on the disk"; break;
    case ERROR_ALREADY_EXISTS: msg = "Cannot create a file when that file already exists"; break;
    case ERROR_NO_MORE_ITEMS: msg = "No more data is available"; break;
    case ERROR_CANCELLED: msg = "The operation was canceled by the user"; break;
    case ERROR_TIMEOUT: msg = "This operation returned because the timeout period expired"; break;
    case ERROR_MORE_DATA: msg = "More data is available"; break;
    case ERROR_INVALID_PARAMETER: msg = "The parameter is incorrect"; break;
    case ERROR_INVALID_HANDLE: msg = "The handle is invalid"; break;
    case ERROR_NOT_ENOUGH_MEMORY: msg = "Not enough memory resources are available"; break;
    default:
        if (err == 0)
            msg = "Success";
        else
            msg = NULL;
        break;
    }
    if (msg != NULL)
        snprintf(err_string, sizeof(err_string), "%s", msg);
    else
        snprintf(err_string, sizeof(err_string), "Unknown error 0x%08X", err);
    return err_string;
}

/* NB: StrError() itself lives in utils.c (it is the upstream storage error
 * mapper, not a plain errno lookup). */

/* ------------------------------------------------------------------ */
/* Handle machinery                                                    */
/* ------------------------------------------------------------------ */
#define HANDLE_MAGIC 0x52554655u    /* 'RUFU' */

typedef enum {
    HK_FILE = 0,
    HK_EVENT,
    HK_THREAD,
    HK_MUTEX,
    HK_TIMER
} handle_kind_t;

typedef struct posix_handle {
    uint32_t magic;
    handle_kind_t kind;
    int fd;
    uint64_t pos;
    uint64_t offset;        /* partition base offset (logical handles) */
    char path[MAX_PATH];
    BOOL writable;
    BOOL is_device;
    /* synchronous event */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    BOOL signalled;
    BOOL manual;
    /* thread */
    pthread_t thread;
    LPTHREAD_START_ROUTINE fn;
    void* thread_param;
    DWORD thread_ret;
    BOOL thread_exited;
    DWORD thread_id;
    /* timer */
    TIMERPROC timer_proc;
    UINT timer_interval;
    UINT_PTR timer_id;
    volatile int timer_stop;
    pthread_t timer_thread;
} posix_handle_t;

static __thread DWORD fake_thread_id = 0;

static BOOL valid_handle(HANDLE h)
{
    posix_handle_t* ph = (posix_handle_t*)h;
    return (ph != NULL) && (ph != INVALID_HANDLE_VALUE) && (ph->magic == HANDLE_MAGIC);
}

static posix_handle_t* new_handle(handle_kind_t kind)
{
    posix_handle_t* ph = (posix_handle_t*)calloc(1, sizeof(posix_handle_t));
    if (ph == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    ph->magic = HANDLE_MAGIC;
    ph->kind = kind;
    ph->fd = -1;
    return ph;
}

/* ------------------------------------------------------------------ */
/* Core file API                                                       */
/* ------------------------------------------------------------------ */
static int access_to_oflag(DWORD access)
{
    if ((access & (GENERIC_READ | GENERIC_WRITE)) == (GENERIC_READ | GENERIC_WRITE))
        return O_RDWR;
    if (access & GENERIC_WRITE)
        return O_WRONLY;
    return O_RDONLY;
}

static DWORD errno_to_winerror(int err)
{
    switch (err) {
    case 0:                    return ERROR_SUCCESS;
    case ENOENT: case ENOTDIR: return ERROR_FILE_NOT_FOUND;
    case EACCES: case EPERM:   return ERROR_ACCESS_DENIED;
    case EROFS:                return ERROR_WRITE_PROTECT;
    case EBUSY:                return ERROR_BUSY;
    case EEXIST:               return ERROR_FILE_EXISTS;
    case ENOSPC: case EDQUOT:  return ERROR_DISK_FULL;
    case EISDIR:               return ERROR_ACCESS_DENIED;
    case EINVAL:               return ERROR_INVALID_PARAMETER;
    case EIO:                  return ERROR_CRC;
    case ENOMEM:               return ERROR_NOT_ENOUGH_MEMORY;
    case EMFILE: case ENFILE:  return ERROR_NOT_ENOUGH_QUOTA;
    case ENODEV: case ENXIO:   return ERROR_SECTOR_NOT_FOUND;
    case EAGAIN:               return ERROR_BUSY;
    case ENAMETOOLONG:         return ERROR_INVALID_NAME;
    default:                   return ERROR_GEN_FAILURE;
    }
}

/*
 * Apply Windows file attributes (read-only, hidden, system, archive) on a
 * file of a FAT volume. The FAT attribute bits are the same as Windows'.
 * Setting the system attribute requires CAP_LINUX_IMMUTABLE, so retry
 * without it when running unprivileged. Not an error on other file systems.
 */
#include <linux/msdos_fs.h>
static void set_fat_attributes(int fd, DWORD attributes)
{
    uint32_t attr, cur = 0;

    attributes &= (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    if (attributes == 0)
        return;
    if (ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &cur) != 0)
        return;
    attr = cur | attributes;
    if (ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attr) != 0 && (attributes & FILE_ATTRIBUTE_SYSTEM)) {
        attr &= ~FILE_ATTRIBUTE_SYSTEM;
        if (ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attr) != 0)
            return;
    }
}

HANDLE CreateFileA(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    int flags, fd;
    struct stat st;
    posix_handle_t* ph;
    BOOL exclusive, have_stat = FALSE;
    (void)lpSecurityAttributes;
    (void)hTemplateFile;

    if (lpFileName == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    flags = access_to_oflag(dwDesiredAccess);
    switch (dwCreationDisposition) {
    case CREATE_NEW:        flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS:     flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     break;
    case OPEN_ALWAYS:       flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    flags |= O_CLOEXEC;

    /* "no write sharing" on a device maps to Linux exclusive open (O_EXCL) */
    exclusive = ((dwDesiredAccess & GENERIC_WRITE) != 0) && (dwShareMode == 0);

    if (exclusive) {
        have_stat = (stat(lpFileName, &st) == 0);
        if (have_stat && S_ISBLK(st.st_mode))
            flags |= O_EXCL;
    }

    fd = open(lpFileName, flags, 0666);
    if (fd < 0) {
        if ((errno == EACCES || errno == EBUSY) && exclusive) {
            /* Retry without O_EXCL only if not a device (regular file
             * exclusive semantic is not what callers want). */
            if (have_stat && S_ISREG(st.st_mode)) {
                flags &= ~O_EXCL;
                fd = open(lpFileName, flags, 0666);
            }
        }
        if (fd < 0) {
            SetLastError(errno_to_winerror(errno));
            return INVALID_HANDLE_VALUE;
        }
    }

    ph = new_handle(HK_FILE);
    if (ph == NULL) {
        close(fd);
        return INVALID_HANDLE_VALUE;
    }
    ph->fd = fd;
    ph->writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    ph->pos = 0;
    ph->offset = 0;
    snprintf(ph->path, sizeof(ph->path), "%s", lpFileName);
    if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode))
        ph->is_device = TRUE;
    /* Windows applies the attributes of a newly created file (on FAT these
     * are the directory entry attributes, e.g. for KERNEL.SYS/ldlinux.sys) */
    if ((flags & O_CREAT) && S_ISREG(st.st_mode))
        set_fat_attributes(fd, dwFlagsAndAttributes);
    SetLastError(ERROR_SUCCESS);
    return (HANDLE)ph;
}

/* Partition-relative handle support (Linux port): I/O is shifted by
 * this offset. Used when a partition device node is unavailable (image
 * files) or when the caller opened a whole-disk handle. */
void SetHandleOffset(HANDLE hFile, uint64_t offset)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    if (valid_handle(hFile)) {
        ph->offset = offset;
        ph->pos = 0;
    }
}

uint64_t GetHandleOffset(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    return valid_handle(hFile) ? ph->offset : 0;
}

const char* GetHandlePath(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    return valid_handle(hFile) ? ph->path : "";
}

/* Underlying OS file descriptor (used by the device layer for ioctls) */
int GetHandleFD(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    return valid_handle(hFile) ? ph->fd : -1;
}

/* Size of the underlying file/device excluding any logical offset */
uint64_t GetHandleRawSize(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    struct stat st;
    uint64_t size = 0;
    if (!valid_handle(hFile) || ph->kind != HK_FILE)
        return 0;
    if (ph->is_device) {
        if (ioctl(ph->fd, BLKGETSIZE64, &size) == 0)
            return size;
    }
    if (fstat(ph->fd, &st) != 0)
        return 0;
    return (uint64_t)st.st_size;
}

HANDLE CreateFileU(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    /* Paths are UTF-8 on Linux: same function */
    return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    ssize_t r;
    uint64_t offset;

    (void)lpOverlapped;
    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (lpNumberOfBytesRead != NULL)
        *lpNumberOfBytesRead = 0;
    offset = ((lpOverlapped != NULL) ? lpOverlapped->Offset : ph->pos) + ph->offset;

    r = pread(ph->fd, lpBuffer, nNumberOfBytesToRead, (off_t)offset);
    if (r < 0) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    /* As on Windows, a synchronous read at the end of a file succeeds with
     * zero bytes read: only overlapped reads fail with ERROR_HANDLE_EOF */
    if (r == 0 && nNumberOfBytesToRead > 0 && lpOverlapped != NULL) {
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    if (lpNumberOfBytesRead != NULL)
        *lpNumberOfBytesRead = (DWORD)r;
    if (lpOverlapped == NULL)
        ph->pos = offset - ph->offset + (uint64_t)r;   /* pos is relative to the partition */
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    const uint8_t* buf = (const uint8_t*)lpBuffer;
    DWORD total = 0;
    uint64_t offset;

    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!ph->writable) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    offset = ((lpOverlapped != NULL) ? lpOverlapped->Offset : ph->pos) + ph->offset;

    while (total < nNumberOfBytesToWrite) {
        ssize_t w = pwrite(ph->fd, buf + total, nNumberOfBytesToWrite - total,
                           (off_t)(offset + total));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (total > 0)
                break;  /* partial write: report what we did */
            SetLastError(errno_to_winerror(errno));
            if (lpNumberOfBytesWritten != NULL)
                *lpNumberOfBytesWritten = 0;
            return FALSE;
        }
        total += (DWORD)w;
    }
    if (lpNumberOfBytesWritten != NULL)
        *lpNumberOfBytesWritten = total;
    if (lpOverlapped == NULL)
        ph->pos = offset - ph->offset + total;          /* pos is relative to the partition */
    if (total != nNumberOfBytesToWrite) {
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/* Port of upstream stdio.c WriteFileWithRetry() semantics */
BOOL WriteFileWithRetry(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                        LPDWORD lpNumberOfBytesWritten, DWORD nRetries)
{
    DWORD i, size;
    BOOL r;

    for (i = 0; i < nRetries; i++) {
        size = 0;
        r = WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, &size, NULL);
        if (r && size == nNumberOfBytesToWrite) {
            if (lpNumberOfBytesWritten != NULL)
                *lpNumberOfBytesWritten = size;
            return TRUE;
        }
        /* If the tail was partially written (common when hitting the end of
         * a smaller device), do not retry the whole buffer. */
        if (r || (size != 0))
            break;
        Sleep(200);
    }
    if (lpNumberOfBytesWritten != NULL)
        *lpNumberOfBytesWritten = size;
    return FALSE;
}

BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                      PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    uint64_t new_pos;

    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (dwMoveMethod) {
    case FILE_BEGIN:   new_pos = (uint64_t)liDistanceToMove.QuadPart; break;
    case FILE_CURRENT: new_pos = ph->pos + (uint64_t)liDistanceToMove.QuadPart; break;
    case FILE_END: {
        new_pos = GetHandleRawSize(hFile) - ph->offset +
                  (uint64_t)liDistanceToMove.QuadPart;
        break;
    }
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ph->pos = new_pos;
    if (lpNewFilePointer != NULL)
        lpNewFilePointer->QuadPart = (int64_t)new_pos;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh,
                     DWORD dwMoveMethod)
{
    LARGE_INTEGER li;
    li.QuadPart = lDistanceToMove;
    if (lpDistanceToMoveHigh != NULL)
        li.HighPart = *lpDistanceToMoveHigh;
    if (!SetFilePointerEx(hFile, li, NULL, dwMoveMethod))
        return INVALID_SET_FILE_POINTER;
    return (DWORD)((posix_handle_t*)hFile)->pos;
}

BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    struct stat st;

    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    lpFileSize->QuadPart = (int64_t)(GetHandleRawSize(hFile) - ph->offset);
    return TRUE;
}

DWORD GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh)
{
    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li))
        return INVALID_FILE_SIZE;
    if (lpFileSizeHigh != NULL)
        *lpFileSizeHigh = (DWORD)li.HighPart;
    return (DWORD)li.LowPart;
}

BOOL SetEndOfFile(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (ph->is_device)
        return TRUE;    /* no-op on devices, like Windows */
    if (ftruncate(ph->fd, (off_t)ph->pos) != 0) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (fsync(ph->fd) != 0 && errno != EINVAL && errno != EROFS) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL CloseHandle(HANDLE hObject)
{
    posix_handle_t* ph = (posix_handle_t*)hObject;
    if (!valid_handle(hObject)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (ph->kind) {
    case HK_FILE:
        if (ph->fd >= 0)
            close(ph->fd);
        break;
    case HK_EVENT:
    case HK_MUTEX:
        pthread_mutex_destroy(&ph->mutex);
        pthread_cond_destroy(&ph->cond);
        break;
    case HK_THREAD:
        /* detach-join semantics: wait for the thread to finish */
        if (!ph->thread_exited)
            pthread_join(ph->thread, NULL);
        break;
    case HK_TIMER:
        ph->timer_stop = 1;
        pthread_join(ph->timer_thread, NULL);
        break;
    }
    ph->magic = 0;
    free(ph);
    return TRUE;
}

DWORD GetFileType(HANDLE hFile)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    if (!valid_handle(hFile))
        return FILE_TYPE_UNKNOWN;
    return FILE_TYPE_DISK;
}

int _openU(const char* filename, int oflag, int pmode)
{
    /* MSVCRT's _S_IREAD/_S_IWRITE are mapped to POSIX permissions; the
     * process umask applies, as with any newly created file */
    if (pmode == 0)
        pmode = 0666;
    return open(filename, oflag | O_CLOEXEC, pmode);
}

/* ------------------------------------------------------------------ */
/* File management helpers                                             */
/* ------------------------------------------------------------------ */
BOOL DeleteFileA(const char* lpFileName)
{
    if (unlink(lpFileName) == 0)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}
BOOL DeleteFileU(const char* lpFileName) { return DeleteFileA(lpFileName); }

BOOL MoveFileA(const char* lpExistingFileName, const char* lpNewFileName)
{
    if (rename(lpExistingFileName, lpNewFileName) == 0)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}
BOOL MoveFileU(const char* a, const char* b) { return MoveFileA(a, b); }
BOOL MoveFileExU(const char* a, const char* b, DWORD flags)
{
    (void)flags;
    return MoveFileA(a, b);
}

BOOL CopyFileA(const char* lpExistingFileName, const char* lpNewFileName, BOOL bFailIfExists)
{
    int in_fd, out_fd;
    ssize_t r, w;
    char buf[1 << 16];

    if (bFailIfExists && access(lpNewFileName, F_OK) == 0) {
        SetLastError(ERROR_FILE_EXISTS);
        return FALSE;
    }
    in_fd = open(lpExistingFileName, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    out_fd = open(lpNewFileName, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out_fd < 0) {
        SetLastError(errno_to_winerror(errno));
        close(in_fd);
        return FALSE;
    }
    while ((r = read(in_fd, buf, sizeof(buf))) > 0) {
        char* p = buf;
        while (r > 0) {
            w = write(out_fd, p, (size_t)r);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                SetLastError(errno_to_winerror(errno));
                close(in_fd);
                close(out_fd);
                return FALSE;
            }
            p += w;
            r -= w;
        }
    }
    if (r < 0) {
        SetLastError(errno_to_winerror(errno));
        close(in_fd);
        close(out_fd);
        return FALSE;
    }
    close(in_fd);
    close(out_fd);
    return TRUE;
}
BOOL CopyFileU(const char* a, const char* b, BOOL f) { return CopyFileA(a, b, f); }

BOOL CreateDirectoryA(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    (void)lpSecurityAttributes;
    if (mkdir(lpPathName, 0777) == 0 || errno == EEXIST)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}
BOOL CreateDirectoryU(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    return CreateDirectoryA(lpPathName, lpSecurityAttributes);
}
/* Recursive directory creation ('mkdir -p'). Returns 0 (ERROR_SUCCESS) or
 * ERROR_ALREADY_EXISTS on success, as the Windows shell API does. */
int SHCreateDirectoryExU(HWND hwnd, const char* pszPath, const void* psa)
{
    char tmp[MAX_PATH], *p;
    BOOL existed;

    (void)hwnd; (void)psa;
    if (pszPath == NULL || pszPath[0] == 0)
        return ERROR_INVALID_PARAMETER;
    existed = PathIsDirectoryU(pszPath);
    snprintf(tmp, sizeof(tmp), "%s", pszPath);
    for (p = tmp + 1; *p != 0; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return (int)errno_to_winerror(errno);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return (int)errno_to_winerror(errno);
    return existed ? ERROR_ALREADY_EXISTS : 0;
}

/* Recursively delete a directory (upstream stdfn.c SHDeleteDirectoryExU) */
#include <ftw.h>
static int rm_entry(const char* path, const struct stat* sb, int flag, struct FTW* ftwbuf)
{
    (void)sb; (void)flag; (void)ftwbuf;
    return remove(path);
}
int SHDeleteDirectoryExU(HWND hwnd, const char* pszPath, FILEOP_FLAGS fFlags)
{
    (void)hwnd; (void)fFlags;
    if (pszPath == NULL || pszPath[0] == 0 || strcmp(pszPath, "/") == 0)
        return ERROR_INVALID_PARAMETER;
    return (nftw(pszPath, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0) ? 0 : (int)errno_to_winerror(errno);
}

BOOL RemoveDirectoryA(const char* lpPathName)
{
    if (rmdir(lpPathName) == 0)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}

DWORD GetFileAttributesA(const char* lpFileName)
{
    struct stat st;
    if (stat(lpFileName, &st) != 0) {
        SetLastError(errno_to_winerror(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    if (S_ISDIR(st.st_mode))
        return FILE_ATTRIBUTE_DIRECTORY;
    return FILE_ATTRIBUTE_NORMAL;
}
DWORD GetFileAttributesU(const char* lpFileName) { return GetFileAttributesA(lpFileName); }
BOOL SetFileAttributesU(const char* lpFileName, DWORD dwFileAttributes)
{
    int fd = open(lpFileName, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return TRUE;    /* Not on a FAT file system, or no such file: ignore, as before */
    set_fat_attributes(fd, dwFileAttributes);
    close(fd);
    return TRUE;
}
BOOL PathFileExistsU(const char* szPath)
{
    struct stat st;
    return (stat(szPath, &st) == 0);
}
BOOL PathIsDirectoryU(const char* pszPath)
{
    struct stat st;
    return (stat(pszPath, &st) == 0) && S_ISDIR(st.st_mode);
}

DWORD GetFullPathNameU(const char* lpFileName, DWORD nBufferLength, char* lpBuffer, char** lpFilePart)
{
    char* resolved = realpath_dup(lpFileName);
    if (resolved == NULL) {
        SetLastError(errno_to_winerror(errno));
        return 0;
    }
    if ((DWORD)strlen(resolved) + 1 > nBufferLength) {
        DWORD needed = (DWORD)strlen(resolved) + 1;
        free(resolved);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return needed;
    }
    strcpy(lpBuffer, resolved);
    free(resolved);
    if (lpFilePart != NULL) {
        char* sep = strrchr(lpBuffer, '/');
        *lpFilePart = (sep != NULL) ? sep + 1 : lpBuffer;
    }
    return (DWORD)strlen(lpBuffer);
}

BOOL CreateSymbolicLinkU(const char* lpSymlinkFileName, const char* lpTargetFileName, DWORD dwFlags)
{
    (void)dwFlags;
    if (symlink(lpTargetFileName, lpSymlinkFileName) == 0)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}

static void stat_to_filetime(const struct stat* st, FILETIME* ft)
{
    /* Unix epoch (1970) to Windows epoch (1601): add 11644473600 seconds */
    if (ft == NULL)
        return;
    uint64_t t = ((uint64_t)st->st_mtime + 11644473600ULL) * 10000000ULL;
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

BOOL GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    struct stat st;
    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (fstat(ph->fd, &st) != 0) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    stat_to_filetime(&st, lpCreationTime);
    stat_to_filetime(&st, lpLastAccessTime);
    stat_to_filetime(&st, lpLastWriteTime);
    return TRUE;
}

BOOL SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime)
{
    posix_handle_t* ph = (posix_handle_t*)hFile;
    const FILETIME* ft = lpLastWriteTime ? lpLastWriteTime : lpLastAccessTime;
    struct timespec times[2];
    uint64_t t;
    (void)lpCreationTime;

    if (!valid_handle(hFile) || ph->kind != HK_FILE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (ft == NULL)
        return TRUE;
    t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (t < 11644473600ULL * 10000000ULL)
        t = 11644473600ULL * 10000000ULL;
    t -= 11644473600ULL * 10000000ULL;
    times[0].tv_sec = t / 10000000ULL;
    times[0].tv_nsec = (t % 10000000ULL) * 100;
    times[1] = times[0];
    if (futimens(ph->fd, times) != 0) {
        SetLastError(errno_to_winerror(errno));
        return FALSE;
    }
    return TRUE;
}

DWORD GetTempPathU(DWORD nBufferLength, char* lpBuffer)
{
    const char* tmp = getenv("TMPDIR");
    size_t len;
    if (tmp == NULL || *tmp == 0)
        tmp = "/tmp";
    len = strlen(tmp) + 2;  /* trailing slash */
    if (lpBuffer == NULL || nBufferLength < len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return (DWORD)len;
    }
    snprintf(lpBuffer, nBufferLength, "%s/", tmp);
    return (DWORD)strlen(lpBuffer);
}

DWORD GetTempFileNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempFileName)
{
    /* As Windows: create a unique, empty file and return a nonzero value */
    char tmpl[MAX_PATH];
    size_t len = strlen(lpPathName);
    int fd;

    (void)uUnique;
    snprintf(tmpl, sizeof(tmpl), "%s%s%sXXXXXX", lpPathName,
             (len > 0 && lpPathName[len - 1] == '/') ? "" : "/", lpPrefixString);
    fd = mkstemp(tmpl);
    if (fd < 0) {
        SetLastError(errno_to_winerror(errno));
        return 0;
    }
    close(fd);
    snprintf(lpTempFileName, MAX_PATH, "%s", tmpl);
    return 1;
}

/* Create a unique temporary directory (upstream stdfn.c GetTempDirNameU) */
DWORD GetTempDirNameU(char* lpPathName, char* lpPrefixString, UINT uUnique, char* lpTempDirName)
{
    char tmpl[MAX_PATH];
    size_t len = strlen(lpPathName);

    (void)uUnique;
    snprintf(tmpl, sizeof(tmpl), "%s%s%sXXXXXX", lpPathName,
             (len > 0 && lpPathName[len - 1] == '/') ? "" : "/", lpPrefixString);
    if (mkdtemp(tmpl) == NULL) {
        SetLastError(errno_to_winerror(errno));
        return 0;
    }
    snprintf(lpTempDirName, MAX_PATH, "%s", tmpl);
    return 1;
}
DWORD GetModuleFileNameA(HMODULE hModule, char* lpFilename, DWORD nSize)
{
    ssize_t r;
    (void)hModule;
    r = readlink("/proc/self/exe", lpFilename, nSize - 1);
    if (r <= 0) {
        SetLastError(errno_to_winerror(errno));
        return 0;
    }
    lpFilename[r] = 0;
    return (DWORD)r;
}
DWORD GetModuleFileNameU(HMODULE hModule, char* lpFilename, DWORD nSize)
{
    return GetModuleFileNameA(hModule, lpFilename, nSize);
}

DWORD GetCurrentDirectoryU(DWORD nBufferLength, char* lpBuffer)
{
    if (getcwd(lpBuffer, nBufferLength) == NULL) {
        SetLastError(errno_to_winerror(errno));
        return 0;
    }
    return (DWORD)strlen(lpBuffer);
}

BOOL SetCurrentDirectoryU(const char* lpPathName)
{
    if (chdir(lpPathName) == 0)
        return TRUE;
    SetLastError(errno_to_winerror(errno));
    return FALSE;
}

int _chdirU(const char* dirname) { return chdir(dirname); }
int _mkdirU(const char* dirname)
{
    if (mkdir(dirname, 0777) == 0)
        return 0;
    return -1;
}
int _rmdirU(const char* dirname) { return rmdir(dirname); }
int _unlinkU(const char* path) { return unlink(path); }
int _accessU(const char* path, int mode)
{
    int flags = F_OK;
    if (mode & 4) flags |= R_OK;
    if (mode & 2) flags |= W_OK;
    return access(path, flags);
}
int _stat64U(const char* path, struct stat* buf)
{
    return stat(path, buf);
}
uint64_t _filesizeU(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (uint64_t)st.st_size;
}

/* FindFirstFile emulation (upstream stdio.c uses it for ISO listing) */
typedef struct find_handle {
    uint32_t magic;
    DIR* dir;
    char pattern[MAX_PATH];
    char dir_path[MAX_PATH];
    WIN32_FIND_DATAA data;
} find_handle_t;

static BOOL find_next_internal(find_handle_t* fh)
{
    struct dirent* de;
    (void)fh->data;
    while ((de = readdir(fh->dir)) != NULL) {
        char full[MAX_PATH * 2];
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", fh->dir_path, de->d_name);
        if (stat(full, &st) != 0)
            continue;
        memset(&fh->data, 0, sizeof(fh->data));
        fh->data.dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        fh->data.nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        fh->data.nFileSizeLow = (DWORD)st.st_size;
        snprintf(fh->data.cFileName, MAX_PATH, "%s", de->d_name);
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

HANDLE FindFirstFileU(const char* lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    find_handle_t* fh;
    char* sep;

    fh = (find_handle_t*)calloc(1, sizeof(*fh));
    if (fh == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    snprintf(fh->pattern, sizeof(fh->pattern), "%s", lpFileName);
    /* Strip trailing wildcard */
    sep = strrchr(fh->pattern, '/');
    if (sep != NULL) {
        *sep = 0;
        snprintf(fh->dir_path, sizeof(fh->dir_path), "%s", fh->pattern);
    } else {
        snprintf(fh->dir_path, sizeof(fh->dir_path), ".");
    }
    fh->dir = opendir(fh->dir_path);
    if (fh->dir == NULL) {
        free(fh);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    fh->magic = HANDLE_MAGIC;
    if (!find_next_internal(fh)) {
        closedir(fh->dir);
        free(fh);
        return INVALID_HANDLE_VALUE;
    }
    memcpy(lpFindFileData, &fh->data, sizeof(*lpFindFileData));
    return (HANDLE)fh;
}

BOOL FindNextFileU(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    find_handle_t* fh = (find_handle_t*)hFindFile;
    if (fh == NULL || fh->magic != HANDLE_MAGIC)
        return FALSE;
    if (!find_next_internal(fh))
        return FALSE;
    memcpy(lpFindFileData, &fh->data, sizeof(*lpFindFileData));
    return TRUE;
}

BOOL FindClose(HANDLE hFindFile)
{
    find_handle_t* fh = (find_handle_t*)hFindFile;
    if (fh == NULL || fh->magic != HANDLE_MAGIC)
        return FALSE;
    closedir(fh->dir);
    fh->magic = 0;
    free(fh);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Threads / events / mutexes / timers                                 */
/* ------------------------------------------------------------------ */
static void* thread_trampoline(void* arg)
{
    posix_handle_t* ph = (posix_handle_t*)arg;
    fake_thread_id = ph->thread_id;
    ph->thread_ret = ph->fn(ph->thread_param);
    ph->thread_exited = TRUE;
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                    DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    posix_handle_t* ph;
    pthread_attr_t attr;
    static LONG thread_id_counter = 1000;

    (void)lpThreadAttributes;
    (void)dwCreationFlags;
    ph = new_handle(HK_THREAD);
    if (ph == NULL)
        return NULL;
    ph->fn = lpStartAddress;
    ph->thread_param = lpParameter;
    ph->thread_id = (DWORD)InterlockedIncrement(&thread_id_counter);
    if (lpThreadId != NULL)
        *lpThreadId = ph->thread_id;

    pthread_attr_init(&attr);
    if (dwStackSize > 0 && dwStackSize >= PTHREAD_STACK_MIN)
        pthread_attr_setstacksize(&attr, (size_t)dwStackSize);
    if (pthread_create(&ph->thread, &attr, thread_trampoline, ph) != 0) {
        pthread_attr_destroy(&attr);
        ph->magic = 0;
        free(ph);
        SetLastError(ERROR_CANT_START_THREAD);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return (HANDLE)ph;
}

void ExitThread(DWORD dwExitCode)
{
    (void)dwExitCode;
    pthread_exit(NULL);
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    posix_handle_t* ph = (posix_handle_t*)hHandle;
    if (!valid_handle(hHandle)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    switch (ph->kind) {
    case HK_THREAD:
        if (!ph->thread_exited)
            pthread_join(ph->thread, NULL);
        ph->thread_exited = TRUE;
        return WAIT_OBJECT_0;
    case HK_EVENT:
    case HK_MUTEX: {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += dwMilliseconds / 1000;
        ts.tv_nsec += (long)(dwMilliseconds % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&ph->mutex);
        while (!ph->signalled) {
            if (dwMilliseconds == INFINITE) {
                pthread_cond_wait(&ph->cond, &ph->mutex);
            } else if (pthread_cond_timedwait(&ph->cond, &ph->mutex, &ts) != 0) {
                pthread_mutex_unlock(&ph->mutex);
                return WAIT_TIMEOUT;
            }
        }
        if (!ph->manual)
            ph->signalled = FALSE;
        pthread_mutex_unlock(&ph->mutex);
        return WAIT_OBJECT_0;
    }
    default:
        return WAIT_OBJECT_0;
    }
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds)
{
    uint64_t start = GetTickCount64();
    DWORD i;

    if (bWaitAll) {
        for (i = 0; i < nCount; i++) {
            DWORD remaining = (dwMilliseconds == INFINITE) ? INFINITE :
                (DWORD)MAX(0, (int64_t)dwMilliseconds - (int64_t)(GetTickCount64() - start));
            if (WaitForSingleObject(lpHandles[i], remaining) != WAIT_OBJECT_0)
                return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    }
    for (;;) {
        for (i = 0; i < nCount; i++) {
            posix_handle_t* ph = (posix_handle_t*)lpHandles[i];
            if (valid_handle(lpHandles[i]) && ph->kind == HK_THREAD && ph->thread_exited)
                return i;
            if (valid_handle(lpHandles[i]) && (ph->kind == HK_EVENT || ph->kind == HK_MUTEX) &&
                ph->signalled)
                return i;
        }
        if (dwMilliseconds != INFINITE && (GetTickCount64() - start) >= dwMilliseconds)
            return WAIT_TIMEOUT;
        Sleep(10);
    }
}

BOOL SetEvent(HANDLE hEvent)
{
    posix_handle_t* ph = (posix_handle_t*)hEvent;
    if (!valid_handle(hEvent))
        return FALSE;
    pthread_mutex_lock(&ph->mutex);
    ph->signalled = TRUE;
    pthread_cond_broadcast(&ph->cond);
    pthread_mutex_unlock(&ph->mutex);
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent)
{
    posix_handle_t* ph = (posix_handle_t*)hEvent;
    if (!valid_handle(hEvent))
        return FALSE;
    pthread_mutex_lock(&ph->mutex);
    ph->signalled = FALSE;
    pthread_mutex_unlock(&ph->mutex);
    return TRUE;
}

static HANDLE create_event_common(BOOL bManualReset, BOOL bInitialState)
{
    posix_handle_t* ph = new_handle(HK_EVENT);
    if (ph == NULL)
        return NULL;
    pthread_mutex_init(&ph->mutex, NULL);
    pthread_cond_init(&ph->cond, NULL);
    ph->manual = bManualReset;
    ph->signalled = bInitialState;
    return (HANDLE)ph;
}

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                    BOOL bInitialState, const char* lpName)
{
    (void)lpEventAttributes;
    (void)lpName;
    return create_event_common(bManualReset, bInitialState);
}

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                    BOOL bInitialState, const wchar_t* lpName)
{
    (void)lpEventAttributes;
    (void)lpName;
    return create_event_common(bManualReset, bInitialState);
}

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, const char* lpName)
{
    posix_handle_t* ph;
    pthread_mutexattr_t attr;
    (void)lpMutexAttributes;
    (void)lpName;
    ph = new_handle(HK_MUTEX);
    if (ph == NULL)
        return NULL;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ph->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&ph->cond, NULL);
    if (bInitialOwner)
        pthread_mutex_lock(&ph->mutex);
    return (HANDLE)ph;
}

BOOL ReleaseMutex(HANDLE hMutex)
{
    posix_handle_t* ph = (posix_handle_t*)hMutex;
    if (!valid_handle(hMutex))
        return FALSE;
    pthread_mutex_unlock(&ph->mutex);
    return TRUE;
}

void Sleep(DWORD dwMilliseconds)
{
    struct timespec ts;
    ts.tv_sec = dwMilliseconds / 1000;
    ts.tv_nsec = (long)(dwMilliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

DWORD GetTickCount(void)
{
    return (DWORD)GetTickCount64();
}

uint64_t GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

ULONGLONG GetTickCount64U(void) { return GetTickCount64(); }

DWORD GetCurrentThreadId(void)
{
    if (fake_thread_id != 0)
        return fake_thread_id;
    return (DWORD)(uintptr_t)pthread_self();
}

DWORD GetCurrentProcessId(void)
{
    return (DWORD)getpid();
}

BOOL SetThreadPriority(HANDLE hThread, int nPriority)
{
    (void)hThread;
    (void)nPriority;
    return TRUE;
}

DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)
{
    (void)hThread;
    (void)dwThreadAffinityMask;
    return 0;
}

BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode)
{
    posix_handle_t* ph = (posix_handle_t*)hThread;
    (void)dwExitCode;
    if (!valid_handle(hThread) || ph->kind != HK_THREAD)
        return FALSE;
    pthread_cancel(ph->thread);
    ph->thread_exited = TRUE;
    return TRUE;
}

int GetThreadPriority(HANDLE hThread)
{
    (void)hThread;
    return THREAD_PRIORITY_NORMAL;
}

/* Interlocked operations (GCC builtins) */
LONG InterlockedIncrement(LONG volatile* Addend) { return __sync_add_and_fetch(Addend, 1); }
LONG InterlockedDecrement(LONG volatile* Addend) { return __sync_sub_and_fetch(Addend, 1); }
LONG InterlockedExchange(LONG volatile* Target, LONG Value)
{
    return __sync_lock_test_and_set(Target, Value);
}
LONG64 InterlockedExchange64(LONG64 volatile* Target, LONG64 Value)
{
    return __sync_lock_test_and_set(Target, Value);
}
PVOID InterlockedExchangePointer(PVOID volatile* Target, PVOID Value)
{
    return __sync_lock_test_and_set(Target, Value);
}
LONG InterlockedCompareExchange(LONG volatile* Destination, LONG Exchange, LONG Comparand)
{
    return __sync_val_compare_and_swap(Destination, Comparand, Exchange);
}
LONG64 InterlockedCompareExchange64(LONG64 volatile* Destination, LONG64 Exchange, LONG64 Comparand)
{
    return __sync_val_compare_and_swap(Destination, Comparand, Exchange);
}

/* Critical sections */
void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &attr);
    pthread_mutexattr_destroy(&attr);
}
void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(cs); }
void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(cs); }
void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(cs); }

/* Timers */
/* Timers run their callback from a dedicated thread. They are tracked by
 * (window, ID), so that KillTimer() can stop them, as on Windows. */
#define MAX_TIMERS 16
static struct { HWND hWnd; UINT_PTR id; posix_handle_t* ph; } timers[MAX_TIMERS];
static pthread_mutex_t timers_lock = PTHREAD_MUTEX_INITIALIZER;

static void* timer_trampoline(void* arg)
{
    posix_handle_t* ph = (posix_handle_t*)arg;
    while (!ph->timer_stop) {
        uint32_t elapsed = 0;
        while (!ph->timer_stop && elapsed < ph->timer_interval) {
            Sleep(50);
            elapsed += 50;
        }
        if (!ph->timer_stop && ph->timer_proc != NULL)
            ph->timer_proc(NULL, WM_TIMER, ph->timer_id, GetTickCount());
    }
    return NULL;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent)
{
    posix_handle_t* ph = NULL;
    int i;

    pthread_mutex_lock(&timers_lock);
    for (i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].ph != NULL && timers[i].hWnd == hWnd && timers[i].id == uIDEvent) {
            ph = timers[i].ph;
            timers[i].ph = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&timers_lock);
    if (ph == NULL)
        return FALSE;
    ph->timer_stop = 1;
    if (!pthread_equal(pthread_self(), ph->timer_thread))
        pthread_join(ph->timer_thread, NULL);
    else
        pthread_detach(ph->timer_thread);
    ph->magic = 0;
    free(ph);
    return TRUE;
}

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
{
    posix_handle_t* ph;
    int i;

    if (lpTimerFunc == NULL)
        return 0;
    /* As on Windows, an existing timer with the same ID is replaced */
    KillTimer(hWnd, nIDEvent);
    ph = new_handle(HK_TIMER);
    if (ph == NULL)
        return 0;
    ph->timer_proc = lpTimerFunc;
    ph->timer_id = nIDEvent;
    ph->timer_interval = (uElapse == 0) ? 1000 : uElapse;
    ph->timer_stop = 0;
    pthread_mutex_lock(&timers_lock);
    for (i = 0; i < MAX_TIMERS && timers[i].ph != NULL; i++);
    if (i >= MAX_TIMERS || pthread_create(&ph->timer_thread, NULL, timer_trampoline, ph) != 0) {
        pthread_mutex_unlock(&timers_lock);
        ph->magic = 0;
        free(ph);
        return 0;
    }
    timers[i].hWnd = hWnd;
    timers[i].id = nIDEvent;
    timers[i].ph = ph;
    pthread_mutex_unlock(&timers_lock);
    return nIDEvent;
}

/* _mm_malloc/_mm_free provided by GCC's mm_malloc.h on x86; provide
 * fallbacks for other architectures. */
#if !defined(__x86_64__) && !defined(__i386__)
void* _mm_malloc(size_t size, size_t align)
{
    void* ptr = NULL;
    if (align < sizeof(void*))
        align = sizeof(void*);
    if (posix_memalign(&ptr, align, size) != 0)
        return NULL;
    return ptr;
}
void _mm_free(void* ptr) { free(ptr); }
#else
#include <mm_malloc.h>
#endif

/* ------------------------------------------------------------------ */
/* Asynchronous I/O (upstream winio.h)                                 */
/* ------------------------------------------------------------------ */
#include "winio.h"

typedef struct {
    HANDLE          hFile;          /* synchronous shim handle */
    uint64_t        offset;         /* offset of the next request */
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    BOOL            quit;
    BOOL            pending;        /* a request was queued */
    BOOL            done;           /* the request completed */
    BOOL            write;
    void*           buf;
    DWORD           len;
    DWORD           result;         /* bytes transferred */
    DWORD           error;          /* Windows error code, or 0 */
    BOOL            size_fetched;   /* GetSizeAsync() was called for this request */
} async_fd_t;

static void* async_worker(void* arg)
{
    async_fd_t* fd = (async_fd_t*)arg;
    LARGE_INTEGER li;
    DWORD n;
    BOOL ok;

    pthread_mutex_lock(&fd->lock);
    for (;;) {
        while (!fd->pending && !fd->quit)
            pthread_cond_wait(&fd->cond, &fd->lock);
        if (fd->quit)
            break;
        fd->pending = FALSE;
        pthread_mutex_unlock(&fd->lock);
        li.QuadPart = (LONGLONG)fd->offset;
        n = 0;
        ok = SetFilePointerEx(fd->hFile, li, NULL, FILE_BEGIN);
        if (ok)
            ok = fd->write ? WriteFile(fd->hFile, fd->buf, fd->len, &n, NULL) :
                             ReadFile(fd->hFile, fd->buf, fd->len, &n, NULL);
        pthread_mutex_lock(&fd->lock);
        fd->result = n;
        fd->error = ok ? 0 : GetLastError();
        fd->done = TRUE;
        pthread_cond_broadcast(&fd->cond);
    }
    pthread_mutex_unlock(&fd->lock);
    return NULL;
}

HANDLE CreateFileAsync(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                       DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes)
{
    async_fd_t* fd = calloc(1, sizeof(async_fd_t));
    if (fd == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    fd->hFile = CreateFileU(lpFileName, dwDesiredAccess, dwShareMode, NULL,
                            dwCreationDisposition, dwFlagsAndAttributes, NULL);
    if (fd->hFile == INVALID_HANDLE_VALUE) {
        free(fd);
        return NULL;
    }
    pthread_mutex_init(&fd->lock, NULL);
    pthread_cond_init(&fd->cond, NULL);
    fd->done = TRUE;
    if (pthread_create(&fd->thread, NULL, async_worker, fd) != 0) {
        CloseHandle(fd->hFile);
        free(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return (HANDLE)fd;
}

VOID CloseFileAsync(HANDLE h)
{
    async_fd_t* fd = (async_fd_t*)h;
    if (fd == NULL || h == INVALID_HANDLE_VALUE)
        return;
    pthread_mutex_lock(&fd->lock);
    while (!fd->done)
        pthread_cond_wait(&fd->cond, &fd->lock);
    fd->quit = TRUE;
    pthread_cond_broadcast(&fd->cond);
    pthread_mutex_unlock(&fd->lock);
    pthread_join(fd->thread, NULL);
    CloseHandle(fd->hFile);
    pthread_mutex_destroy(&fd->lock);
    pthread_cond_destroy(&fd->cond);
    free(fd);
}

static BOOL queue_async(async_fd_t* fd, void* buf, DWORD len, BOOL write)
{
    pthread_mutex_lock(&fd->lock);
    while (!fd->done)
        pthread_cond_wait(&fd->cond, &fd->lock);
    fd->buf = buf;
    fd->len = len;
    fd->write = write;
    fd->done = FALSE;
    fd->size_fetched = FALSE;
    fd->pending = TRUE;
    pthread_cond_broadcast(&fd->cond);
    pthread_mutex_unlock(&fd->lock);
    return TRUE;
}

BOOL ReadFileAsync(HANDLE h, LPVOID lpBuffer, DWORD nNumberOfBytesToRead)
{
    return queue_async((async_fd_t*)h, lpBuffer, nNumberOfBytesToRead, FALSE);
}

BOOL WriteFileAsync(HANDLE h, LPVOID lpBuffer, DWORD nNumberOfBytesToWrite)
{
    return queue_async((async_fd_t*)h, lpBuffer, nNumberOfBytesToWrite, TRUE);
}

BOOL WaitFileAsync(HANDLE h, DWORD dwTimeout)
{
    async_fd_t* fd = (async_fd_t*)h;
    struct timespec ts;
    int rc = 0;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += dwTimeout / 1000;
    ts.tv_nsec += (long)(dwTimeout % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&fd->lock);
    while (!fd->done && rc == 0)
        rc = (dwTimeout == INFINITE) ? pthread_cond_wait(&fd->cond, &fd->lock) :
                                       pthread_cond_timedwait(&fd->cond, &fd->lock, &ts);
    pthread_mutex_unlock(&fd->lock);
    if (rc != 0)
        SetLastError(ERROR_TIMEOUT);
    return (rc == 0);
}

BOOL GetSizeAsync(HANDLE h, LPDWORD lpNumberOfBytes)
{
    async_fd_t* fd = (async_fd_t*)h;
    BOOL r;

    pthread_mutex_lock(&fd->lock);
    while (!fd->done)
        pthread_cond_wait(&fd->cond, &fd->lock);
    if (fd->size_fetched) {
        pthread_mutex_unlock(&fd->lock);
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    fd->size_fetched = TRUE;
    *lpNumberOfBytes = fd->result;
    r = (fd->error == 0) || (fd->error == ERROR_HANDLE_EOF);
    if (r)
        fd->offset += fd->result;
    else
        SetLastError(fd->error);
    pthread_mutex_unlock(&fd->lock);
    return r;
}

HANDLE GetAsyncFileHandle(HANDLE h)
{
    return ((async_fd_t*)h)->hFile;
}

/*
 * MSVCRT _open_osfhandle(): bled (compressed images) works on file
 * descriptors. Hand it a duplicate of the handle's descriptor, positioned
 * at the handle's current (absolute) offset. O_DIRECT is cleared since bled
 * does not use aligned buffers (the sector_write() callback takes care of
 * writing whole sectors to devices).
 */
int _open_osfhandle(intptr_t osfhandle, int flags)
{
    posix_handle_t* ph = (posix_handle_t*)osfhandle;
    int fd, fl;

    (void)flags;
    if (!valid_handle((HANDLE)ph) || ph->kind != HK_FILE) {
        errno = EBADF;
        return -1;
    }
    fd = dup(ph->fd);
    if (fd < 0)
        return -1;
#ifdef O_DIRECT
    fl = fcntl(fd, F_GETFL);
    if ((fl >= 0) && (fl & O_DIRECT))
        fcntl(fd, F_SETFL, fl & ~O_DIRECT);
#else
    (void)fl;
#endif
    if (lseek(fd, (off_t)(ph->offset + ph->pos), SEEK_SET) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
