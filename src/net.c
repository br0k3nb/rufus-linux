/*
 * Rufus: The Reliable USB Formatting Utility
 * Networking functionality (web file download, signed file validation)
 * Copyright © 2012-2025 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Rufus Linux port notes
 * ----------------------
 * - Upstream uses WinINet. The port uses libcurl, loaded at runtime with
 *   dlopen() so that neither the curl development headers nor a hard link
 *   dependency are needed (libcurl.so.4 is present on all desktop distros).
 *   Only the stable, long-standing parts of the libcurl ABI are used.
 *   If libcurl can't be loaded, the curl(1) executable is used instead.
 * - ValidateOpensslSignature() uses OpenSSL's libcrypto instead of the
 *   Windows CryptoAPI, with the exact same RSA-2048/SHA-256 public key that
 *   upstream uses to validate the files from https://rufus.ie/files.
 */

#include "rfus.h"
#include "localization.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

DWORD DownloadStatus = 0;

/* ------------------------------------------------------------------ */
/* Minimal libcurl ABI (from curl/curl.h - these values never change)  */
/* ------------------------------------------------------------------ */
typedef void CURL;
typedef int CURLcode;
#define CURLE_OK                    0
#define CURL_GLOBAL_DEFAULT         3
#define CURLOPTTYPE_LONG            0
#define CURLOPTTYPE_OBJECTPOINT     10000
#define CURLOPTTYPE_FUNCTIONPOINT   20000
#define CURLOPTTYPE_OFF_T           30000
#define CURLOPT_WRITEDATA           (CURLOPTTYPE_OBJECTPOINT + 1)
#define CURLOPT_URL                 (CURLOPTTYPE_OBJECTPOINT + 2)
#define CURLOPT_USERAGENT           (CURLOPTTYPE_OBJECTPOINT + 18)
#define CURLOPT_WRITEFUNCTION       (CURLOPTTYPE_FUNCTIONPOINT + 11)
#define CURLOPT_XFERINFOFUNCTION    (CURLOPTTYPE_FUNCTIONPOINT + 219)
#define CURLOPT_XFERINFODATA        (CURLOPTTYPE_OBJECTPOINT + 57)
#define CURLOPT_NOPROGRESS          (CURLOPTTYPE_LONG + 43)
#define CURLOPT_FOLLOWLOCATION      (CURLOPTTYPE_LONG + 52)
#define CURLOPT_FAILONERROR         (CURLOPTTYPE_LONG + 45)
#define CURLOPT_CONNECTTIMEOUT      (CURLOPTTYPE_LONG + 78)
#define CURLOPT_LOW_SPEED_LIMIT     (CURLOPTTYPE_LONG + 19)
#define CURLOPT_LOW_SPEED_TIME      (CURLOPTTYPE_LONG + 20)
#define CURLOPT_NOBODY              (CURLOPTTYPE_LONG + 44)
#define CURLOPT_ERRORBUFFER         (CURLOPTTYPE_OBJECTPOINT + 10)
#define CURLINFO_LONG               0x200000
#define CURLINFO_RESPONSE_CODE      (CURLINFO_LONG + 2)
#define CURL_ERROR_SIZE             256

static struct {
    void* lib;
    BOOL tried;
    CURLcode (*global_init)(long);
    CURL* (*easy_init)(void);
    CURLcode (*easy_setopt)(CURL*, int, ...);
    CURLcode (*easy_perform)(CURL*);
    CURLcode (*easy_getinfo)(CURL*, int, ...);
    void (*easy_cleanup)(CURL*);
    const char* (*easy_strerror)(CURLcode);
} curl;

static BOOL load_curl(void)
{
    static const char* names[] = { "libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4", NULL };
    int i;

    if (curl.tried)
        return (curl.lib != NULL);
    curl.tried = TRUE;
    for (i = 0; names[i] != NULL && curl.lib == NULL; i++)
        curl.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (curl.lib == NULL)
        return FALSE;
    curl.global_init = (CURLcode (*)(long))dlsym(curl.lib, "curl_global_init");
    curl.easy_init = (CURL* (*)(void))dlsym(curl.lib, "curl_easy_init");
    curl.easy_setopt = (CURLcode (*)(CURL*, int, ...))dlsym(curl.lib, "curl_easy_setopt");
    curl.easy_perform = (CURLcode (*)(CURL*))dlsym(curl.lib, "curl_easy_perform");
    curl.easy_getinfo = (CURLcode (*)(CURL*, int, ...))dlsym(curl.lib, "curl_easy_getinfo");
    curl.easy_cleanup = (void (*)(CURL*))dlsym(curl.lib, "curl_easy_cleanup");
    curl.easy_strerror = (const char* (*)(CURLcode))dlsym(curl.lib, "curl_easy_strerror");
    if (!curl.global_init || !curl.easy_init || !curl.easy_setopt || !curl.easy_perform ||
        !curl.easy_getinfo || !curl.easy_cleanup || !curl.easy_strerror) {
        dlclose(curl.lib);
        curl.lib = NULL;
        return FALSE;
    }
    curl.global_init(CURL_GLOBAL_DEFAULT);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Download                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    FILE* fd;               /* destination file (or NULL for a buffer) */
    uint8_t* buf;           /* destination buffer */
    uint64_t size;          /* bytes received */
    uint64_t alloc;         /* allocated buffer size */
    const char* short_name;
    BOOL failed;
} dl_ctx_t;

static size_t dl_write(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    dl_ctx_t* ctx = (dl_ctx_t*)userdata;
    size_t len = size * nmemb;

    if (IS_ERROR(ErrorStatus))  /* user cancel */
        return 0;
    if (ctx->fd != NULL) {
        if (fwrite(ptr, 1, len, ctx->fd) != len) {
            ctx->failed = TRUE;
            return 0;
        }
    } else {
        if (ctx->size + len + 1 > ctx->alloc) {
            uint64_t new_alloc = (ctx->alloc == 0) ? 64 * KB : ctx->alloc;
            uint8_t* nbuf;
            while (new_alloc < ctx->size + len + 1)
                new_alloc *= 2;
            nbuf = realloc(ctx->buf, (size_t)new_alloc);
            if (nbuf == NULL) {
                ctx->failed = TRUE;
                return 0;
            }
            ctx->buf = nbuf;
            ctx->alloc = new_alloc;
        }
        memcpy(&ctx->buf[ctx->size], ptr, len);
    }
    ctx->size += len;
    return len;
}

static int dl_progress(void* userdata, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
    (void)ultotal; (void)ulnow; (void)userdata;
    if (IS_ERROR(ErrorStatus))
        return 1;   /* abort the transfer */
    if (dltotal > 0)
        UpdateProgressWithInfo(OP_NOOP, MSG_241, (uint64_t)dlnow, (uint64_t)dltotal);
    return 0;
}

/* Fallback when libcurl is unavailable: the curl(1) executable */
static BOOL download_with_curl_cli(const char* url, const char* user_agent, dl_ctx_t* ctx)
{
    int pipefd[2];
    pid_t pid;
    char buf[64 * 1024];
    ssize_t n;
    int status;

    if (pipe(pipefd) != 0)
        return FALSE;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return FALSE;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("curl", "curl", "-sSfL", "--connect-timeout", "15", "-A", user_agent, url, (char*)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (dl_write(buf, 1, (size_t)n, ctx) != (size_t)n)
            break;
    }
    close(pipefd[0]);
    if (n > 0)
        kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* curl -f exits with 22 on HTTP errors >= 400 */
        DownloadStatus = (WIFEXITED(status) && WEXITSTATUS(status) == 22) ? 404 : 500;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            uprintf("Neither libcurl nor the curl executable are available - can not download");
        return FALSE;
    }
    DownloadStatus = 200;
    return !ctx->failed;
}

/*
 * Download a file from an URL to a file or a buffer. Returns the size of the
 * downloaded data, or 0 on error. When downloading to a buffer, the buffer
 * is NUL terminated (but the NUL is not included in the returned size).
 * Same semantics as upstream (net.c).
 */
uint64_t DownloadToFileOrBufferEx(const char* url, const char* file, const char* user_agent,
    uint8_t** buffer, HWND hProgressDialog, BOOL bTaskBarProgress)
{
    const char* short_name;
    char errbuf[CURL_ERROR_SIZE] = "";
    dl_ctx_t ctx = { 0 };
    BOOL r = FALSE;
    long code = 0;
    CURL* h = NULL;

    (void)hProgressDialog; (void)bTaskBarProgress;
    DownloadStatus = 404;
    if (url == NULL || (file == NULL && buffer == NULL))
        return 0;
    if (buffer != NULL)
        *buffer = NULL;
    if (user_agent == NULL)
        user_agent = APPLICATION_NAME "/" RUFUS_VERSION_STR " (Linux)";

    short_name = (file != NULL) ? file : url;
    if (strrchr(short_name, '/') != NULL)
        short_name = strrchr(short_name, '/') + 1;
    ctx.short_name = short_name;

    if (file != NULL)
        PrintInfo(0, MSG_085, short_name);
    uprintf("Downloading '%s'", url);

    if (file != NULL) {
        ctx.fd = fopen(file, "wb");
        if (ctx.fd == NULL) {
            uprintf("Unable to create file '%s': %s", file, strerror(errno));
            goto out;
        }
    }

    if (load_curl()) {
        h = curl.easy_init();
        if (h == NULL)
            goto out;
        curl.easy_setopt(h, CURLOPT_URL, url);
        curl.easy_setopt(h, CURLOPT_USERAGENT, user_agent);
        curl.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
        curl.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
        curl.easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
        curl.easy_setopt(h, CURLOPT_WRITEFUNCTION, dl_write);
        curl.easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
        curl.easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl.easy_setopt(h, CURLOPT_XFERINFOFUNCTION, dl_progress);
        curl.easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
        CURLcode cr = curl.easy_perform(h);
        curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        DownloadStatus = (DWORD)code;
        if (cr != CURLE_OK) {
            if (!IS_ERROR(ErrorStatus))
                uprintf("Download failed: %s", (errbuf[0] != 0) ? errbuf : curl.easy_strerror(cr));
            goto out;
        }
        if (code < 200 || code >= 300) {
            uprintf("Unable to access file: HTTP status %ld", code);
            goto out;
        }
        if (ctx.failed)
            goto out;
        r = TRUE;
    } else {
        r = download_with_curl_cli(url, user_agent, &ctx);
        if (!r)
            uprintf("Unable to download '%s'", url);
    }

    if (r) {
        DownloadStatus = 200;
        uprintf("Successfully downloaded '%s' (%s)", short_name, SizeToHumanReadable(ctx.size, FALSE, FALSE));
        if (buffer != NULL) {
            if (ctx.buf == NULL)
                ctx.buf = calloc(1, 1);
            if (ctx.buf != NULL)
                ctx.buf[ctx.size] = 0;
            *buffer = ctx.buf;
            ctx.buf = NULL;
        }
    }

out:
    if (h != NULL)
        curl.easy_cleanup(h);
    if (ctx.fd != NULL) {
        fclose(ctx.fd);
        if (!r)
            unlink(file);
    }
    free(ctx.buf);
    if (!r && IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED))
        uprintf("Download cancelled");
    return r ? ctx.size : 0;
}

/* ------------------------------------------------------------------ */
/* Signature validation (port of upstream pki.c)                       */
/* ------------------------------------------------------------------ */

/* The RSA public key modulus for the private key used to sign the files on
 * the Rufus server (identical to upstream pki.c - with the leading 0x00 that
 * openssl adds to force a positive integer). Exponent is 65537. */
static const uint8_t rsa_pubkey_modulus[] = {
    0x00, 0xb6, 0x40, 0x7d, 0xd1, 0x98, 0x7b, 0x81, 0x9e, 0xbe, 0x23, 0x0f, 0x32, 0x5d, 0x55,
    0x60, 0xc6, 0xbf, 0xb4, 0x41, 0xbb, 0x43, 0x1b, 0xf1, 0xe1, 0xe6, 0xf9, 0x2b, 0xd6, 0xdd,
    0x11, 0x50, 0xe8, 0xb9, 0x3f, 0x19, 0x97, 0x5e, 0xa7, 0x8b, 0x4a, 0x30, 0xc6, 0x76, 0x58,
    0x72, 0x1c, 0xac, 0xff, 0xa1, 0xf8, 0x96, 0x6c, 0x51, 0x5d, 0x13, 0x11, 0xe3, 0x5b, 0x11,
    0x82, 0xf5, 0x9a, 0x69, 0xe4, 0x28, 0x97, 0x0f, 0xca, 0x1f, 0x02, 0xea, 0x1f, 0x7d, 0xdc,
    0xf9, 0xfc, 0x79, 0x2f, 0x61, 0xff, 0x8e, 0x45, 0x60, 0x65, 0xba, 0x37, 0x9b, 0xde, 0x49,
    0x05, 0x6a, 0xa8, 0xfd, 0x70, 0xd0, 0x0c, 0x79, 0xb6, 0xd7, 0x81, 0xaa, 0x54, 0xc3, 0xc6,
    0x4a, 0x87, 0xa0, 0x45, 0xee, 0xca, 0xd5, 0xd5, 0xc5, 0xc2, 0xac, 0x86, 0x42, 0xb3, 0x58,
    0x27, 0xd2, 0x43, 0xb9, 0x37, 0xf2, 0xe6, 0x75, 0x66, 0x17, 0x53, 0xd0, 0x38, 0xd0, 0xc6,
    0x57, 0xc2, 0x55, 0x36, 0xa2, 0x43, 0x87, 0xea, 0x24, 0xf0, 0x96, 0xec, 0x34, 0xdd, 0x79,
    0x4d, 0x80, 0x54, 0x9d, 0x84, 0x81, 0xa7, 0xcf, 0x0c, 0xa5, 0x7c, 0xd6, 0x63, 0xfa, 0x7a,
    0x66, 0x30, 0xa9, 0x50, 0xee, 0xf0, 0xe5, 0xf8, 0xa2, 0x2d, 0xac, 0xfc, 0x24, 0x21, 0xfe,
    0xef, 0xe8, 0xd3, 0x6f, 0x0e, 0x27, 0xb0, 0x64, 0x22, 0x95, 0x3e, 0x6d, 0xa6, 0x66, 0x97,
    0xc6, 0x98, 0xc2, 0x47, 0xb3, 0x98, 0x69, 0x4d, 0xb1, 0xb5, 0xd3, 0x6f, 0x43, 0xf5, 0xd7,
    0xa5, 0x13, 0x5e, 0x8c, 0x28, 0x4f, 0x62, 0x4e, 0x01, 0x48, 0x0a, 0x63, 0x89, 0xe7, 0xca,
    0x34, 0xaa, 0x7d, 0x2f, 0xbb, 0x70, 0xe0, 0x31, 0xbb, 0x39, 0x49, 0xa3, 0xd2, 0xc9, 0x2e,
    0xa6, 0x30, 0x54, 0x9a, 0x5c, 0x4d, 0x58, 0x17, 0xd9, 0xfc, 0x3a, 0x43, 0xe6, 0x8e, 0x2a,
    0x18, 0xe9
};

/* Validate an RSA-2048 PKCS#1 v1.5 SHA-256 signature, as created by
 * 'openssl dgst -sha256 -sign private.pem -out file.sig file' */
BOOL ValidateOpensslSignature(BYTE* pbBuffer, DWORD dwBufferLen, BYTE* pbSignature, DWORD dwSigLen)
{
    BIGNUM *n = NULL, *e = NULL;
    OSSL_PARAM_BLD* bld = NULL;
    OSSL_PARAM* params = NULL;
    EVP_PKEY_CTX* pctx = NULL;
    EVP_PKEY* pkey = NULL;
    EVP_MD_CTX* mdctx = NULL;
    BOOL r = FALSE;

    n = BN_bin2bn(rsa_pubkey_modulus, sizeof(rsa_pubkey_modulus), NULL);
    e = BN_new();
    if (n == NULL || e == NULL || !BN_set_word(e, 65537))
        goto out;
    bld = OSSL_PARAM_BLD_new();
    if (bld == NULL || !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) ||
        !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
        goto out;
    params = OSSL_PARAM_BLD_to_param(bld);
    pctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    if (params == NULL || pctx == NULL || EVP_PKEY_fromdata_init(pctx) <= 0 ||
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        uprintf("PKI: Could not import public key");
        goto out;
    }
    mdctx = EVP_MD_CTX_new();
    if (mdctx == NULL || EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) <= 0 ||
        EVP_DigestVerifyUpdate(mdctx, pbBuffer, dwBufferLen) <= 0)
        goto out;
    r = (EVP_DigestVerifyFinal(mdctx, pbSignature, dwSigLen) == 1);
    if (!r) {
        /* As upstream: don't keep potentially nasty stuff in memory */
        memset(pbBuffer, 0, dwBufferLen);
        uprintf("Signature validation failed");
    }

out:
    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);
    return r;
}

/* Download and validate a signed file. The file must have a corresponding
 * '.sig' on the server. */
DWORD DownloadSignedFile(const char* url, const char* file, HWND hProgressDialog, BOOL bPromptOnError)
{
    char* url_sig = NULL;
    uint8_t *buf = NULL, *sig = NULL;
    DWORD buf_len = 0, sig_len = 0;
    DWORD ret = 0;
    FILE* fd = NULL;

    url_sig = malloc(strlen(url) + 5);
    if (url_sig == NULL) {
        uprintf("Could not allocate signature URL");
        goto out;
    }
    strcpy(url_sig, url);
    strcat(url_sig, ".sig");

    buf_len = (DWORD)DownloadToFileOrBuffer(url, NULL, &buf, hProgressDialog, FALSE);
    if (buf_len == 0)
        goto out;
    sig_len = (DWORD)DownloadToFileOrBuffer(url_sig, NULL, &sig, NULL, FALSE);
    if ((sig_len != RSA_SIGNATURE_SIZE) || (!ValidateOpensslSignature(buf, buf_len, sig, sig_len))) {
        uprintf("FATAL: Download signature is invalid ✗");
        DownloadStatus = 403;   /* Forbidden */
        ErrorStatus = RUFUS_ERROR(APPERR(ERROR_BAD_SIGNATURE));
        goto out;
    }

    uprintf("Download signature is valid ✓");
    DownloadStatus = 206;   /* Partial content */
    fd = fopen(file, "wb");
    if (fd == NULL) {
        uprintf("Unable to create file '%s': %s", file, strerror(errno));
        goto out;
    }
    if (fwrite(buf, 1, buf_len, fd) != buf_len) {
        uprintf("Error writing file '%s': %s", file, strerror(errno));
        goto out;
    }
    ret = buf_len;
    DownloadStatus = 200;   /* Full content */

out:
    if (fd != NULL) {
        fclose(fd);
        if (ret == 0)
            unlink(file);
    }
    if ((bPromptOnError) && (DownloadStatus != 200)) {
        PrintInfo(0, MSG_242);
        Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_044),
            IS_ERROR(ErrorStatus) ? StrError(ErrorStatus, FALSE) : lmprintf(MSG_242));
    }
    free(url_sig);
    free(buf);
    free(sig);
    return ret;
}

/* Check that an URL can be accessed (HEAD request) */
BOOL IsDownloadable(const char* url)
{
    long code = 0;
    CURL* h;
    BOOL r;

    if (url == NULL || !load_curl())
        return FALSE;
    h = curl.easy_init();
    if (h == NULL)
        return FALSE;
    curl.easy_setopt(h, CURLOPT_URL, url);
    curl.easy_setopt(h, CURLOPT_NOBODY, 1L);
    curl.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    r = (curl.easy_perform(h) == CURLE_OK);
    curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl.easy_cleanup(h);
    return r && (code >= 200) && (code < 300);
}
