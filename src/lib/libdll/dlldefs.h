/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1997-2018 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *               Glenn Fowler <glenn.s.fowler@gmail.com>                *
 *                                                                      *
 ***********************************************************************/

// : : generated by proto : :
// : : generated from ast/src/lib/libdll/features/dll by iffe version 2013-11-14 : :

#ifndef _def_dll_dll
#define _def_dll_dll 1

#include <fts.h>

#if _hdr_dlfcn
#include <dlfcn.h>
#endif

#include "ast.h"
#include "cdt.h"

#ifndef RTLD_PARENT
#define RTLD_PARENT 0
#endif

#define DLL_INFO_PREVER 0x0001  // pre-suffix style version
#define DLL_INFO_DOTVER 0x0002  // post-suffix style version

typedef unsigned long (*Dll_plugin_version_f)(void);
typedef int (*Dllerror_f)(void *, void *, int, ...);

typedef struct Dllinfo_s {
    char **sibling;  // sibling dirs on $PATH
    char *prefix;    // library name prefix
    char *suffix;    // library name suffix
    char *env;       // library path env var
    int flags;       // DLL_INFO_* flags
    char *sib[3];
    char sibbuf[64];
    char envbuf[64];
} Dllinfo_t;

typedef struct Dllnames_s {
    char *id;
    char *name;
    char *base;
    char *type;
    char *opts;
    char *path;
    char data[1024];
} Dllnames_t;

typedef struct Dllent_s {
    char *path;
    char *name;
} Dllent_t;

typedef struct Uniq_s {
    Dtlink_t link;
    char name[1];
} Uniq_t;

typedef struct Dllscan_s {
    Dllent_t entry;
    Uniq_t *uniq;
    int flags;
    Dt_t *dict;
    Dtdisc_t disc;
    FTS *fts;
    FTSENT *ent;
    Sfio_t *tmp;
    char **sb;
    char **sp;
    char *pb;
    char *pp;
    char *pe;
    int off;
    int prelen;
    int suflen;
    char **lib;
    char nam[64];
    char pat[64];
    char buf[64];
} Dllscan_t;

extern Dllinfo_t *dllinfo(void);
extern void *dllplugin(const char *, const char *, const char *, unsigned long, unsigned long *,
                       int, char *, size_t);
extern void *dllplug(const char *, const char *, const char *, int, char *, size_t);
extern void *dllfind(const char *, const char *, int, char *, size_t);
extern void *dllopen(const char *, int);
extern void *dllnext(int);
extern void *dlllook(void *, const char *);
extern int dllcheck(void *, const char *, unsigned long, unsigned long *);
extern unsigned long dllversion(void *, const char *);
extern char *dllerror(int);

extern Dllscan_t *dllsopen(const char *, const char *, const char *);
extern Dllent_t *dllsread(Dllscan_t *);
extern int dllsclose(Dllscan_t *);

#endif
