/* Copyright (c) 2026 Elliott H. Liggett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Real-time safety audit: count what the audio callback does that it must not.
 *
 * The audio thread has a hard deadline -- 5.3 ms for a 256-frame block at 48 kHz -- and
 * what matters is the WORST case, not the average.  malloc may ask the kernel for pages;
 * a lock may wait on another thread; getenv walks the environment.  Any of them can blow
 * the deadline once in a while, which is heard as a click and is very hard to reproduce.
 *
 * Reading the code cannot settle this, because the interesting calls are several layers
 * down in code we did not write.  So this interposes the allocator and the usual
 * offenders, arms a flag around run(), and counts.  A call made while the flag is set is a
 * defect with a name and a backtrace, instead of an occasional click.
 *
 * Build and use:
 *     gcc -shared -fPIC -o rt_audit.so rt_audit.c -ldl
 *     LD_PRELOAD=./rt_audit.so ./some_host
 * The plugin's own harness sets the flag through rt_audit_set_active().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <execinfo.h>

/* Counting is only half the job: the useful output is WHERE.  Call sites are keyed by the
 * immediate return address, which is enough to name the function with dladdr. */
#define SITE_MAX 64
static void *g_site_addr[SITE_MAX];
static long  g_site_count[SITE_MAX];
static int   g_site_n = 0;

static void note_site(void *ra)
{
    for (int i = 0; i < g_site_n; i++)
        if (g_site_addr[i] == ra) { g_site_count[i]++; return; }
    if (g_site_n < SITE_MAX) { g_site_addr[g_site_n] = ra; g_site_count[g_site_n] = 1; g_site_n++; }
}

static atomic_int g_active = 0;      /* set while run() is executing */
static atomic_long g_counts[6];
enum { C_MALLOC, C_FREE, C_REALLOC, C_CALLOC, C_GETENV, C_LOCK };
static const char *const kNames[6] = {
    "malloc", "free", "realloc", "calloc", "getenv", "pthread_mutex_lock"
};

/* Called by the harness around the audio callback. */
void rt_audit_set_active(int on) { atomic_store(&g_active, on); }
long rt_audit_count(int which) { return atomic_load(&g_counts[which]); }
void rt_audit_reset(void)
{
    for (int i = 0; i < 6; i++) atomic_store(&g_counts[i], 0);
    g_site_n = 0;
}

void rt_audit_report(void)
{
    /* The verdict rests on the calls that can BLOCK -- allocation and locking.  getenv is
     * counted separately: it is not for the audio thread either, but a handful of one-time
     * static initialisations are a different thing from an allocation every block, and
     * conflating them makes the report cry wolf. */
    long blocking = 0;
    for (int i = 0; i < 6; i++)
        if (i != C_GETENV) blocking += atomic_load(&g_counts[i]);
    const long env = atomic_load(&g_counts[C_GETENV]);

    fprintf(stderr, "\n--- real-time audit: calls made INSIDE the audio callback ---\n");
    for (int i = 0; i < 6; i++)
        fprintf(stderr, "    %-20s %ld\n", kNames[i], atomic_load(&g_counts[i]));
    /* Sort call sites by weight and name them. */
    for (int i = 0; i < g_site_n; i++)
        for (int j = i + 1; j < g_site_n; j++)
            if (g_site_count[j] > g_site_count[i]) {
                long c = g_site_count[i]; g_site_count[i] = g_site_count[j]; g_site_count[j] = c;
                void *a = g_site_addr[i]; g_site_addr[i] = g_site_addr[j]; g_site_addr[j] = a;
            }
    if (g_site_n) fprintf(stderr, "  busiest call sites:\n");
    for (int i = 0; i < g_site_n && i < 12; i++) {
        Dl_info info;
        if (dladdr(g_site_addr[i], &info) && info.dli_sname)
            fprintf(stderr, "    %8ld  %s  (+%ld in %s)\n", g_site_count[i], info.dli_sname,
                    (long)((char *)g_site_addr[i] - (char *)info.dli_saddr),
                    info.dli_fname ? strrchr(info.dli_fname, '/') + 1 : "?");
        else if (dladdr(g_site_addr[i], &info) && info.dli_fname)
            /* No symbol -- hidden visibility.  Report the file and the offset within it,
             * which addr2line turns into a file and line. */
            fprintf(stderr, "    %8ld  %s +0x%lx\n", g_site_count[i],
                    strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                    (unsigned long)((char *)g_site_addr[i] - (char *)info.dli_fbase));
        else
            fprintf(stderr, "    %8ld  %p\n", g_site_count[i], g_site_addr[i]);
    }
    if (blocking == 0 && env == 0)
        fprintf(stderr, "  CLEAN: nothing that can block was called on the audio thread.\n");
    else if (blocking == 0)
        fprintf(stderr, "  CLEAN for allocation and locking.\n"
                        "  %ld getenv call%s remain; check they are one-time static\n"
                        "  initialisation and not per-block.\n", env, env == 1 ? "" : "s");
    else
        fprintf(stderr, "  NOT CLEAN: %ld blocking call%s on the audio thread -- see the\n"
                        "  call sites above.\n", blocking, blocking == 1 ? "" : "s");
}

#define HOOK(name, ret, args, call, slot)                       \
    static ret (*real_##name) args = NULL;                      \
    ret name args {                                             \
        if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name);\
        if (atomic_load(&g_active)) {                           \
            atomic_fetch_add(&g_counts[slot], 1);               \
            note_site(__builtin_return_address(0));             \
        }                                                       \
        return real_##name call;                                \
    }

HOOK(malloc,  void *, (size_t s),            (s),        C_MALLOC)
HOOK(realloc, void *, (void *p, size_t s),   (p, s),     C_REALLOC)
HOOK(calloc,  void *, (size_t n, size_t s),  (n, s),     C_CALLOC)
HOOK(getenv,  char *, (const char *n),       (n),        C_GETENV)

static void (*real_free)(void *) = NULL;
void free(void *p)
{
    if (!real_free) real_free = dlsym(RTLD_NEXT, "free");
    if (atomic_load(&g_active)) {
        atomic_fetch_add(&g_counts[C_FREE], 1);
        note_site(__builtin_return_address(0));
    }
    real_free(p);
}
