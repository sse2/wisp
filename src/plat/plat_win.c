#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <io.h>
#include <share.h>
#include <shlobj.h>
#include <wincrypt.h>

#include "plat.h"

#include "common.h"

#include <stdlib.h>
#include <string.h>

static wchar_t *to_wide(const char *s) {
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *from_wide(const wchar_t *w) {
    if (!w)
        return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char *s = malloc((size_t)n);
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

struct wisp_thread {
    HANDLE handle;
};
struct wisp_mutex {
    CRITICAL_SECTION cs;
};
struct wisp_cond {
    CONDITION_VARIABLE cv;
};

typedef struct {
    void (*entry)(void *);
    void *arg;
} thread_start;

static DWORD WINAPI thread_thunk(LPVOID p) {
    thread_start s = *(thread_start *)p;
    free(p);
    s.entry(s.arg);
    return 0;
}

wisp_thread *wisp_thread_start(void (*entry)(void *), void *arg) {
    wisp_thread *t = malloc(sizeof *t);
    thread_start *s = malloc(sizeof *s);
    if (!t || !s) {
        free(t);
        free(s);
        return NULL;
    }
    s->entry = entry;
    s->arg = arg;
    t->handle = CreateThread(NULL, 0, thread_thunk, s, 0, NULL);
    if (!t->handle) {
        free(t);
        free(s);
        return NULL;
    }
    return t;
}

void wisp_thread_join(wisp_thread *t) {
    if (!t)
        return;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

wisp_mutex *wisp_mutex_new(void) {
    wisp_mutex *m = malloc(sizeof *m);
    if (m)
        InitializeCriticalSection(&m->cs);
    return m;
}

void wisp_mutex_free(wisp_mutex *m) {
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void wisp_mutex_lock(wisp_mutex *m) { EnterCriticalSection(&m->cs); }
void wisp_mutex_unlock(wisp_mutex *m) { LeaveCriticalSection(&m->cs); }

wisp_cond *wisp_cond_new(void) {
    wisp_cond *c = malloc(sizeof *c);
    if (c)
        InitializeConditionVariable(&c->cv);
    return c;
}

void wisp_cond_free(wisp_cond *c) { free(c); }

void wisp_cond_wait(wisp_cond *c, wisp_mutex *m) {
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}

bool wisp_cond_wait_ms(wisp_cond *c, wisp_mutex *m, uint32_t ms) {
    return SleepConditionVariableCS(&c->cv, &m->cs, ms) != 0;
}

void wisp_cond_signal(wisp_cond *c) { WakeConditionVariable(&c->cv); }
void wisp_cond_broadcast(wisp_cond *c) { WakeAllConditionVariable(&c->cv); }

uint64_t wisp_now_ms(void) { return GetTickCount64(); }
void wisp_sleep_ms(uint32_t ms) { Sleep(ms); }

static char *known_dir(int csidl) {
    wchar_t buf[MAX_PATH];
    if (SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, buf) != S_OK)
        return NULL;
    return from_wide(buf);
}

char *wisp_dir_path(wisp_dir which) {
    char *base = NULL;
    char *path = NULL;
    switch (which) {
    case WISP_DIR_CONFIG:
        base = known_dir(CSIDL_APPDATA);
        path = wisp_path_join(base, "wisp");
        break;
    case WISP_DIR_DATA:
        base = known_dir(CSIDL_LOCAL_APPDATA);
        path = wisp_path_join(base, "wisp");
        break;
    case WISP_DIR_CACHE:
        base = known_dir(CSIDL_LOCAL_APPDATA);
        path = wisp_path_join(base, "wisp\\cache");
        break;
    }
    free(base);
    if (path)
        wisp_mkdirs(path);
    return path;
}

FILE *wisp_fopen(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path);
    wchar_t *wm = to_wide(mode);
    FILE *f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

FILE *wisp_fopen_shared(const char *path, const char *mode) {
    wchar_t *wp = to_wide(path);
    wchar_t *wm = to_wide(mode);
    FILE *f = (wp && wm) ? _wfsopen(wp, wm, _SH_DENYNO) : NULL;
    free(wp);
    free(wm);
    return f;
}

bool wisp_mkdirs(const char *path) {
    wchar_t *w = to_wide(path);
    if (!w)
        return false;
    for (wchar_t *p = w; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p != w && *(p - 1) != L':') {
            wchar_t saved = *p;
            *p = 0;
            CreateDirectoryW(w, NULL);
            *p = saved;
        }
    }
    bool ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok;
}

void wisp_dir_list(const char *dir, void (*cb)(void *ctx, const char *name), void *ctx) {
    char *pat = wisp_aprintf("%s\\*", dir);
    wchar_t *wp = to_wide(pat);
    free(pat);
    if (!wp)
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char *name = from_wide(fd.cFileName);
        if (name) {
            cb(ctx, name);
            free(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool wisp_file_stat(const char *path, int64_t *out_size, int64_t *out_mtime) {
    wchar_t *w = to_wide(path);
    if (!w)
        return false;
    WIN32_FILE_ATTRIBUTE_DATA d;
    bool ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d) != 0;
    free(w);
    if (!ok)
        return false;
    if (out_size)
        *out_size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (out_mtime) {
        ULARGE_INTEGER t;
        t.LowPart = d.ftLastWriteTime.dwLowDateTime;
        t.HighPart = d.ftLastWriteTime.dwHighDateTime;
        *out_mtime = (int64_t)(t.QuadPart / 10000000ull);
    }
    return true;
}

bool wisp_file_delete(const char *path) {
    wchar_t *w = to_wide(path);
    if (!w)
        return false;
    bool ok = DeleteFileW(w) != 0;
    free(w);
    return ok;
}

void wisp_plat_system_cas(void (*add_der)(void *ctx, const unsigned char *der, size_t len),
                          void *ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store)
        return;
    PCCERT_CONTEXT cert = NULL;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL)
        add_der(ctx, cert->pbCertEncoded, cert->cbCertEncoded);
    CertCloseStore(store, 0);
}

char *wisp_plat_ca_bundle_path(void) { return NULL; }

bool wisp_file_write(const char *path, const void *data, size_t len, bool private_perms) {
    (void)private_perms;
    char *tmp = wisp_aprintf("%s.tmp", path);
    if (!tmp)
        return false;
    FILE *f = wisp_fopen(tmp, "wb");
    bool ok = f != NULL;
    if (ok && len)
        ok = fwrite(data, 1, len, f) == len;
    if (f) {
        fflush(f);
        _commit(_fileno(f));
        fclose(f);
    }
    if (ok) {
        wchar_t *wt = to_wide(tmp);
        wchar_t *wp = to_wide(path);
        ok = wt && wp &&
             MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        free(wt);
        free(wp);
    }
    free(tmp);
    return ok;
}

#endif
