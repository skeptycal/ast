/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1985-2013 AT&T Intellectual Property          *
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
 *                    David Korn <dgkorn@gmail.com>                     *
 *                     Phong Vo <phongvo@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
/*
 * string interface to confstr(),pathconf(),sysconf(),sysinfo()
 * extended to allow some features to be set per-process
 */
#include "config_ast.h"  // IWYU pragma: keep

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if _lib_sysinfo
#include <sys/systeminfo.h>
#endif
#include <sys/utsname.h>

#include "ast.h"
#include "ast_errorf.h"
#include "ast_regex.h"
#include "conftab.h"
#include "error.h"
#include "sfio.h"
#include "univlib.h"

#ifdef _pth_getconf_a
#include "proc.h"
#endif

#ifndef DEBUG_astconf
#define DEBUG_astconf 0
#endif

#ifndef _pth_getconf
#undef ASTCONF_system
#define ASTCONF_system 0
#endif

#define CONF_ERROR (CONF_USER << 0)
#define CONF_READONLY (CONF_USER << 1)
#define CONF_ALLOC (CONF_USER << 2)
#define CONF_GLOBAL (CONF_USER << 3)

#define DEFAULT(o) ((state.std || !dynamic[o].ast) ? dynamic[o].std : dynamic[o].ast)
#define INITIALIZE()                                           \
    do {                                                       \
        if (!state.data) astconf_synthesize(NULL, NULL, NULL); \
    } while (0)

#define MAXVAL 256

#if MAXVAL <= UNIV_SIZE
#undef MAXVAL
#define MAXVAL (UNIV_SIZE + 1)
#endif

#ifndef _UNIV_DEFAULT
#define _UNIV_DEFAULT "att"
#endif

static const char *root = "/";

typedef struct Feature_s {
    struct Feature_s *next;
    const char *name;
    char *value;
    char *std;
    char *ast;
    short length;
    short standard;
    unsigned int flags;
    short op;
} Feature_t;

typedef struct Lookup_s {
    Conf_t *conf;
    const char *name;
    unsigned int flags;
    short call;
    short standard;
    short section;
} Lookup_t;

static Feature_t dynamic[] = {
#define OP_architecture 0
    {&dynamic[OP_architecture + 1], "ARCHITECTURE", "", NULL, NULL, 12, CONF_AST, 0,
     OP_architecture},
#define OP_getconf (OP_architecture + 1)
    {&dynamic[OP_getconf + 1], "GETCONF",
#ifdef _pth_getconf
     _pth_getconf,
#else
     "",
#endif
     NULL, NULL, 7, CONF_AST, CONF_READONLY, OP_getconf},
#define OP_hosttype (OP_getconf + 1)
    {&dynamic[OP_hosttype + 1], "HOSTTYPE", HOSTTYPE, NULL, NULL, 8, CONF_AST, CONF_READONLY,
     OP_hosttype},
#define OP_libpath (OP_hosttype + 1)
    {&dynamic[OP_libpath + 1], "LIBPATH",
#ifdef CONF_LIBPATH
     CONF_LIBPATH,
#else
     "",
#endif
     NULL, NULL, 7, CONF_AST, 0, OP_libpath},
#define OP_libprefix (OP_libpath + 1)
    {&dynamic[OP_libprefix + 1], "LIBPREFIX",
#ifdef CONF_LIBPREFIX
     CONF_LIBPREFIX,
#else
     "lib",
#endif
     NULL, NULL, 9, CONF_AST, 0, OP_libprefix},
#define OP_libsuffix (OP_libprefix + 1)
    {&dynamic[OP_libsuffix + 1], "LIBSUFFIX",
#ifdef CONF_LIBSUFFIX
     CONF_LIBSUFFIX,
#else
     ".so",
#endif
     NULL, NULL, 9, CONF_AST, 0, OP_libsuffix},
#define OP_path_attributes (OP_libsuffix + 1)
    {&dynamic[OP_path_attributes + 1], "PATH_ATTRIBUTES", "", "", 0, 15, CONF_AST, CONF_READONLY,
     OP_path_attributes},
#define OP_path_resolve (OP_path_attributes + 1)
    {&dynamic[OP_path_resolve + 1], "PATH_RESOLVE", "", "physical", "metaphysical", 12, CONF_AST, 0,
     OP_path_resolve},
#define OP_universe (OP_path_resolve + 1)
    {NULL, "UNIVERSE", "", "att", NULL, 8, CONF_AST, 0, OP_universe},
    {.next = NULL}};

typedef struct State_s {
    const char *id;
    const char *name;
    const char *strict;
    Feature_t *features;

    int std;

    /* default initialization from here down */

    int prefix;
    int synthesizing;

    char *data;
    char *last;

    Feature_t *recent;

    Ast_confdisc_f notify;

} State_t;

static State_t state = {
    "getconf", "_AST_FEATURES", "POSIXLY_CORRECT", dynamic, -1, 0, 0, NULL, NULL, NULL, NULL};

static_fn char *astconf_feature(Feature_t *, const char *, const char *, const char *, unsigned int,
                                Error_f);

//
// This deliberately uses `strdup()` rather than `fmtbuf()` and thus leaks memory. That should be
// okay given that a) the only consumer of this data is the `getconf` builtin, b) that builtin is
// not enabled by default, and c) even if it is enabled no normal program will invoke it more than a
// few times.
//
// TODO: Rewrite this module to not depend on `fmtbuf()` static buf semantics or just eliminate
// the entire module. See https://github.com/att/ast/issues/962.
//
// static_fn char *astconf_buffer(char *s) { return strcpy(fmtbuf(strlen(s) + 1), s); }
static_fn char *astconf_buffer(char *s) { return strdup(s); }

/*
 * synthesize state for fp
 * fp==0 initializes from getenv(state.name)
 * value==0 just does lookup
 * otherwise state is set to value
 */

static_fn char *astconf_synthesize(Feature_t *fp, const char *path, const char *value) {
    char *s;
    char *d;
    char *v;
    char *p;
    int n;

#if DEBUG_astconf
    if (fp)
        error(-6, "astconf synthesize name=%s path=%s value=%s fp=%p%s", fp->name, path, value, fp,
              state.synthesizing ? " SYNTHESIZING" : "");
#endif
    if (state.synthesizing) {
        if (fp && fp->op < 0 && value && *value) {
            n = strlen(value);
            goto ok;
        }
        return "";
    }
    if (!state.data) {
        char *se;
        char *de;
        char *ve;

        state.prefix = strlen(state.name) + 1;
        n = state.prefix + 3 * MAXVAL;
        s = getenv(state.name);
        if (!s && getenv(state.strict)) s = "standard";
        if (s) n += strlen(s) + 1;
        n = roundof(n, 32);
        state.data = calloc(1, n);
        if (!state.data) return NULL;
        state.last = state.data + n - 1;
        strcpy(state.data, state.name);
        state.data += state.prefix - 1;
        *state.data++ = '=';
        if (s) strcpy(state.data, s);
        ve = state.data;
        state.synthesizing = 1;
        for (;;) {
            for (s = ve; isspace(*s); s++) {
                ;  // empty loop
            }
            for (d = s; *d && !isspace(*d); d++) {
                ;  // empty loop
            }
            for (se = d; isspace(*d); d++) {
                ;  // empty loop
            }
            for (v = d; *v && !isspace(*v); v++) {
                ;  // empty loop
            }
            for (de = v; isspace(*v); v++) {
                ;  // empty loop
            }
            if (!*v) break;
            for (ve = v; *ve && !isspace(*ve); ve++) {
                ;  // empty loop
            }
            if (*ve) {
                *ve = 0;
            } else {
                ve = 0;
            }
            *de = 0;
            *se = 0;
            astconf_feature(0, s, d, v, 0, 0);
            *se = ' ';
            *de = ' ';
            if (!ve) break;
            *ve++ = ' ';
        }
        state.synthesizing = 0;
    }
    if (!fp) return state.data;
    if (!state.last) {
        if (!value) return NULL;
        n = strlen(value);
        goto ok;
    }
    s = (char *)fp->name;
    n = fp->length;
    d = state.data;
    for (;;) {
        while (isspace(*d)) d++;
        if (!*d) break;
        if (!strncmp(d, s, n) && isspace(d[n])) {
            if (!value) {
                for (d += n + 1; *d && !isspace(*d); d++) {
                    ;  // empty loop
                }
                for (; isspace(*d); d++) {
                    ;  // empty loop
                }
                for (s = d; *s && !isspace(*s); s++) {
                    ;  // empty loop
                }
                n = s - d;
                value = (const char *)d;
                goto ok;
            }
            for (s = p = d + n + 1; *s && !isspace(*s); s++) {
                ;  // empty loop
            }
            for (; isspace(*s); s++) {
                ;  // empty loop
            }
            for (v = s; *s && !isspace(*s); s++) {
                ;  // empty loop
            }
            n = s - v;
            if ((!path ||
                 (*path == *p && strlen(path) == (v - p - 1) && !strncmp(path, p, v - p - 1))) &&
                !strncmp(v, value, n)) {
                goto ok;
            }
            for (; isspace(*s); s++) {
                ;  // empty loop
            }
            if (*s) {
                for (; (*d = *s++); d++) {
                    ;  // empty loop
                }
            } else if (d != state.data) {
                d--;
            }
            break;
        }
        for (; *d && !isspace(*d); d++) {
            ;  // empty loop
        }
        for (; isspace(*d); d++) {
            ;  // empty loop
        }
        for (; *d && !isspace(*d); d++) {
            ;  // empty loop
        }
        for (; isspace(*d); d++) {
            ;  // empty loop
        }
        for (; *d && !isspace(*d); d++) {
            ;  // empty loop
        }
    }
    if (!value) {
        if (!fp->op) {
            if (fp->flags & CONF_ALLOC) {
                fp->value[0] = 0;
            } else {
                fp->value = "";
            }
        }
        return 0;
    }
    if (!value[0]) value = "0";
    if (!path || !path[0] || (path[0] == '/' && !path[1])) path = "-";
    n += strlen(path) + strlen(value) + 3;
    if (d + n >= state.last) {
        int c;
        int i;

        i = d - state.data;
        state.data -= state.prefix;
        c = n + state.last - state.data + 3 * MAXVAL;
        c = roundof(c, 32);
        if (state.data) free(state.data);
        state.data = calloc(1, c);
        if (!state.data) return NULL;
        state.last = state.data + c - 1;
        state.data += state.prefix;
        d = state.data + i;
    }
    if (d != state.data) *d++ = ' ';
    for (s = (char *)fp->name; (*d = *s++); d++) {
        ;  // empty loop
    }
    *d++ = ' ';
    for (s = (char *)path; (*d = *s++); d++) {
        ;  // empty loop
    }
    *d++ = ' ';
    for (s = (char *)value; (*d = *s++); d++) {
        ;  // empty loop
    }
#if DEBUG_astconf
    error(-7, "astconf synthesize %s", state.data - state.prefix);
#endif
    sh_setenviron(state.data - state.prefix);
    if (state.notify) (*state.notify)(NULL, NULL, state.data - state.prefix);
    n = s - (char *)value - 1;
ok:
    if (!(fp->flags & CONF_ALLOC)) fp->value = 0;
    if (n == 1 && (*value == '0' || *value == '-')) n = 0;
    if (fp->value) free(fp->value);
    fp->value = calloc(1, n + 1);
    if (!fp->value) {
        fp->value = "";
    } else {
        fp->flags |= CONF_ALLOC;
        memcpy(fp->value, value, n);
        fp->value[n] = 0;
    }
    return fp->value;
}

/*
 * initialize the value for fp
 * if command!=0 then it is checked for on $PATH
 * astconf_synthesize(fp,path,succeed) called on success
 * otherwise astconf_synthesize(fp,path,fail) called
 */

static_fn void astconf_initialize(Feature_t *fp, const char *path, const char *command,
                                  const char *succeed, const char *fail) {
    char *p;
    int ok = 1;

#if DEBUG_astconf
    error(-6, "astconf initialize name=%s path=%s command=%s succeed=%s fail=%s fp=%p%s", fp->name,
          path, command, succeed, fail, fp, state.synthesizing ? " SYNTHESIZING" : "");
#endif
    switch (fp->op) {
        case OP_architecture:
            ok = 1;
            break;
        case OP_hosttype:
            ok = 1;
            break;
        case OP_path_attributes:
            ok = 1;
            break;
        case OP_path_resolve:
            ok = 1;
            break;
        case OP_universe:
            ok = !strcmp(_UNIV_DEFAULT, DEFAULT(OP_universe));
            // FALLTHROUGH
        default: {
            p = getenv("PATH");
            if (!p) break;

#if DEBUG_astconf
            error(-6, "astconf initialize name=%s ok=%d PATH=%s", fp->name, ok, p);
#endif
            Sfio_t *tmp = sfstropen();
            if (!tmp) {
                ok = 1;
                break;
            }

            int r = 1;
            char *d = p;
            for (;;) {
                switch (*p++) {
                    case 0:
                        break;
                    case ':':
                        if (command && fp->op != OP_universe) {
                            r = p - d - 1;
                            if (r) {
                                sfwrite(tmp, d, r);
                                sfputc(tmp, '/');
                                sfputr(tmp, command, 0);
                                if ((d = sfstruse(tmp)) && !eaccess(d, X_OK)) {
                                    ok = 1;
                                    if (fp->op != OP_universe) break;
                                }
                            }
                            d = p;
                        }
                        r = 1;
                        continue;
                    case '/':
                        if (r) {
                            r = 0;
                            if (fp->op == OP_universe) {
                                if (p[0] == 'u' && p[1] == 's' && p[2] == 'r' && p[3] == '/') {
                                    for (p += 4; *p == '/'; p++) {
                                        ;  // empty loop
                                    }
                                }
                                if (p[0] == 'b' && p[1] == 'i' && p[2] == 'n') {
                                    for (p += 3; *p == '/'; p++) {
                                        ;  // empty loop
                                    }
                                    if (!*p || *p == ':') break;
                                }
                            }
                        }
                        if (fp->op == OP_universe) {
                            if (!strncmp(p, "xpg", 3)) {
                                ok = 1;
                                break;
                            }
                            if (!strncmp(p, "bsd", 3) || !strncmp(p, "ucb", 3)) {
                                ok = 0;
                                break;
                            }
                        }
                        continue;
                    default:
                        r = 0;
                        continue;
                }
                break;
            }
            sfclose(tmp);
            break;
        }
    }
#if DEBUG_astconf
    error(-6, "state.std=%d %s [%s] std=%s ast=%s value=%s ok=%d", state.std, fp->name,
          ok ? succeed : fail, fp->std, fp->ast, fp->value, ok);
#endif
    astconf_synthesize(fp, path, ok ? succeed : fail);
}

/*
 * format synthesized value
 */

static_fn char *astconf_format(Feature_t *fp, const char *path, const char *value,
                               unsigned int flags, Error_f conferror) {
    UNUSED(conferror);
    UNUSED(flags);
    int n;
    static struct utsname uts;

#if DEBUG_astconf
    error(-6, "astconf format name=%s path=%s value=%s flags=%04x fp=%p%s", fp->name, path, value,
          flags, fp, state.synthesizing ? " SYNTHESIZING" : "");
#endif
    if (value) {
        fp->flags &= ~CONF_GLOBAL;
    } else if (fp->flags & CONF_GLOBAL) {
        return fp->value;
    }
    switch (fp->op) {
        case OP_architecture:
            if (!uname(&uts)) return fp->value = uts.machine;
            if (!(fp->value = getenv("HOSTNAME"))) fp->value = "unknown";
            break;

        case OP_hosttype:
            break;

        case OP_path_attributes:
            break;

        case OP_path_resolve:
            if (state.synthesizing && value == (char *)fp->std) {
                fp->value = (char *)value;
            } else if (!astconf_synthesize(fp, path, value)) {
                astconf_initialize(fp, path, NULL, "logical", DEFAULT(OP_path_resolve));
            }
            break;

        case OP_universe:
#if _lib_universe
            if (getuniverse(fp->value) < 0) strcpy(fp->value, DEFAULT(OP_universe));
            if (value) setuniverse(value);
#else
#ifdef UNIV_MAX
            n = 0;
            if (value) {
                while (n < univ_max && !!strcmp(value, univ_name[n])) n++;
                if (n >= univ_max) {
                    if (conferror)
                        (*conferror)(&state, &state, 2, "%s: %s: universe value too large",
                                     fp->name, value);
                    return 0;
                }
            }
#ifdef ATT_UNIV
            n = setuniverse(n + 1);
            if (!value && n > 0) setuniverse(n);
#else
            n = universe(value ? n + 1 : U_GET);
#endif
            if (n <= 0 || n >= univ_max) n = 1;
            strcpy(fp->value, univ_name[n - 1]);
#else
            if (value && !strcmp(path, "=")) {
                if (state.synthesizing) {
                    if (!(fp->flags & CONF_ALLOC)) fp->value = 0;
                    n = strlen(value);
                    if (fp->value) free(fp->value);
                    fp->value = calloc(1, n + 1);
                    if (!fp->value) {
                        fp->value = "";
                    } else {
                        fp->flags |= CONF_ALLOC;
                        memcpy(fp->value, value, n);
                        fp->value[n] = 0;
                    }
                } else {
                    astconf_synthesize(fp, path, value);
                }
            } else {
                astconf_initialize(fp, path, "echo", DEFAULT(OP_universe), "ucb");
            }
#endif
#endif
            break;

        default:
            if (state.synthesizing && value == (char *)fp->std) {
                fp->value = (char *)value;
            } else {
                astconf_synthesize(fp, path, value);
            }
            break;
    }
    if (!strcmp(path, "=")) fp->flags |= CONF_GLOBAL;
    return fp->value;
}

/*
 * value==0 get feature name
 * value!=0 set feature name
 * 0 returned if error or not defined; otherwise previous value
 */

static_fn char *astconf_feature(Feature_t *fp, const char *name, const char *path,
                                const char *value, unsigned int flags, Error_f conferror) {
    int n;

    if (value && (!strcmp(value, "-") || !strcmp(value, "0"))) value = "";
    if (!fp) {
        for (fp = state.features; fp && !!strcmp(fp->name, name); fp = fp->next) {
            ;  // empty loop
        }
    }
#if DEBUG_astconf
    error(-6, "astconf feature name=%s path=%s value=%s flags=%04x fp=%p%s", name, path, value,
          flags, fp, state.synthesizing ? " SYNTHESIZING" : "");
#endif
    if (!fp) {
        if (!value) return 0;
        if (state.notify && !(*state.notify)(name, path, value)) return 0;
        n = strlen(name);
        fp = calloc(1, sizeof(Feature_t) + n + 1);
        if (!fp) {
            if (conferror) (*conferror)(&state, &state, 2, "%s: out of space", name);
            return NULL;
        }
        fp->op = -1;
        fp->name = (const char *)fp + sizeof(Feature_t);
        strcpy((char *)fp->name, name);
        fp->length = n;
        fp->std = "";
        fp->next = state.features;
        state.features = fp;
    } else if (value) {
        if (fp->flags & CONF_READONLY) {
            if (conferror) {
                (*conferror)(&state, &state, 2, "%s: cannot set readonly symbol", fp->name);
            }
            return 0;
        }
        if (state.notify && !!strcmp(fp->value, value) && !(*state.notify)(name, path, value)) {
            return 0;
        }
    } else {
        state.recent = fp;
    }
    return astconf_format(fp, path, value, flags, conferror);
}

/*
 * binary search for name in conf[]
 */

static_fn int astconf_lookup(Lookup_t *look, const char *name, unsigned int flags) {
    Conf_t *mid = (Conf_t *)conf;
    Conf_t *lo = mid;
    Conf_t *hi = mid + conf_elements;
    char *e;
    const Prefix_t *p;

    static Conf_t num;

    look->flags = 0;
    look->call = -1;
    look->standard = (flags & ASTCONF_AST) ? CONF_AST : -1;
    look->section = -1;
    while (*name == '_') name++;

again:
    for (p = prefix; p < &prefix[prefix_elements]; p++) {
        if (strncmp(name, p->name, p->length)) continue;
        bool c = name[p->length] == '_' || name[p->length] == '(' || name[p->length] == '#';
        bool v = isdigit(name[p->length]) && name[p->length + 1] == '_';
        if (!c && !v) continue;

        if (p->call < 0) {
            if (look->standard >= 0) break;
            look->standard = p->standard;
        } else {
            if (look->call >= 0) break;
            look->call = p->call;
        }
        if (name[p->length] == '(' || name[p->length] == '#') {
            look->conf = &num;
            strlcpy((char *)num.name, name, sizeof(num.name));
            num.call = p->call;
            num.flags = *name == 'C' ? CONF_STRING : 0;
            num.op = (short)strtol(name + p->length + 1, &e, 10);
            if (name[p->length] == '(' && *e == ')') e++;
            if (*e) break;
            return 1;
        }
        name += p->length + (int)c;
        if (look->section < 0 && !c && v) {
            look->section = name[0] - '0';
            name += 2;
        }
        goto again;
    }

    look->name = name;
#if DEBUG_astconf
    error(-6, "astconf normal name=%s standard=%d section=%d call=%d flags=%04x elements=%d",
          look->name, look->standard, look->section, look->call, flags, conf_elements);
#endif
    int c = *((unsigned char *)name);
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
#if DEBUG_astconf
        error(-7, "astconf lookup name=%s mid=%s", name, mid->name);
#endif
        int v;
        if (!(v = c - *((unsigned char *)mid->name)) && !(v = strcmp(name, mid->name))) {
            hi = mid;
            lo = (Conf_t *)conf;
            do {
                if ((look->standard < 0 || look->standard == mid->standard) &&
                    (look->section < 0 || look->section == mid->section) &&
                    (look->call < 0 || look->call == mid->call)) {
                    goto found;
                }
            } while (mid-- > lo && !strcmp(mid->name, look->name));
            mid = hi;
            hi = lo + conf_elements - 1;
            while (++mid < hi && !strcmp(mid->name, look->name)) {
                if ((look->standard < 0 || look->standard == mid->standard) &&
                    (look->section < 0 || look->section == mid->section) &&
                    (look->call < 0 || look->call == mid->call)) {
                    goto found;
                }
            }
            break;
        } else if (v > 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
found:
    if (look->call < 0 && look->standard >= 0 &&
        (look->section <= 1 || (mid->flags & CONF_MINMAX))) {
        look->flags |= CONF_MINMAX;
    }
    look->conf = mid;
#if DEBUG_astconf
    error(-6, "astconf lookup name=%s standard=%d:%d section=%d:%d call=%d:%d", look->name,
          look->standard, mid->standard, look->section, mid->section, look->call, mid->call);
#endif
    return 1;
}

/*
 * return a tolower'd copy of s
 */

static_fn char *astconf_fmtlower(const char *s) {
    int c;
    char *t;
    char *b;

    b = t = malloc(strlen(s) + 1);
    while ((c = *s++)) {
        *t++ = tolower(c);
    }
    *t = 0;
    return b;
}

/*
 * print value line for p
 * if !name then value prefixed by "p->name="
 * if (flags & CONF_MINMAX) then default minmax value used
 */

static_fn char *astconf_print(Sfio_t *sp, Lookup_t *look, const char *name, const char *path,
                              int listflags, Error_f conferror) {
    Conf_t *p = look->conf;
    unsigned int flags = look->flags;
    Feature_t *fp;
    char *call;
    char *f;
    const char *s;
    int i;
    int n;
    int olderrno;
    int drop;
    int defined;
    int64_t v;
    char buf[PATH_MAX];
    char flg[16];

    if (!name && !(p->flags & CONF_STRING) &&
        (p->flags & (CONF_FEATURE | CONF_LIMIT | CONF_MINMAX)) &&
        (p->flags & (CONF_LIMIT | CONF_PREFIXED)) != CONF_LIMIT) {
        flags |= CONF_PREFIXED;
    }
    olderrno = errno;
    errno = 0;
#if DEBUG_astconf
    error(-5,
          "astconf name=%s:%s:%s standard=%d section=%d call=%s op=%d "
          "flags=|%s%s%s%s%s:|%s%s%s%s%s%s%s%s%s%s",
          name, look->name, p->name, p->standard, p->section, prefix[p->call + CONF_call].name,
          p->op, (flags & CONF_FEATURE) ? "FEATURE|" : "", (flags & CONF_LIMIT) ? "LIMIT|" : "",
          (flags & CONF_MINMAX) ? "MINMAX|" : "", (flags & CONF_PREFIXED) ? "PREFIXED|" : "",
          (flags & CONF_STRING) ? "STRING|" : "", (p->flags & CONF_DEFER_CALL) ? "DEFER_CALL|" : "",
          (p->flags & CONF_DEFER_MM) ? "DEFER_MM|" : "",
          (p->flags & CONF_FEATURE) ? "FEATURE|" : "",
          (p->flags & CONF_LIMIT_DEF) ? "LIMIT_DEF|" : (p->flags & CONF_LIMIT) ? "LIMIT|" : "",
          (p->flags & CONF_MINMAX_DEF) ? "MINMAX_DEF|" : (p->flags & CONF_MINMAX) ? "MINMAX|" : "",
          (p->flags & CONF_NOUNDERSCORE) ? "NOUNDERSCORE|" : "",
          (p->flags & CONF_PREFIXED) ? "PREFIXED|" : "",
          (p->flags & CONF_PREFIX_ONLY) ? "PREFIX_ONLY|" : "",
          (p->flags & CONF_STANDARD) ? "STANDARD|" : "", (p->flags & CONF_STRING) ? "STRING|" : "",
          (p->flags & CONF_UNDERSCORE) ? "UNDERSCORE|" : "");
#endif
    flags |= CONF_LIMIT_DEF | CONF_MINMAX_DEF;
    if (conferror && name) {
        if ((p->flags & CONF_PREFIX_ONLY) && look->standard < 0) goto bad;
        if (!(flags & CONF_MINMAX) || !(p->flags & CONF_MINMAX)) {
            switch (p->call) {
                case CONF_pathconf:
                    // Note: This is deliberately comparing pointers rather than the strings.
                    if (path == root) {
                        (*conferror)(&state, &state, 2, "%s: path expected", name);
                        goto bad;
                    }
                    break;
                default:
                    // Note: This is deliberately comparing pointers rather than the strings.
                    if (path != root) {
                        (*conferror)(&state, &state, 2, "%s: path not expected", name);
                        goto bad;
                    }
                    break;
            }
#ifdef _pth_getconf
            if (p->flags & CONF_DEFER_CALL) goto bad;
#endif
        } else {
            // Note: This is deliberately comparing pointers rather than the strings.
            if (path != root) {
                (*conferror)(&state, &state, 2, "%s: path not expected", name);
                goto bad;
            }
#ifdef _pth_getconf
            if ((p->flags & CONF_DEFER_MM) || !(p->flags & CONF_MINMAX_DEF)) goto bad;
#endif
        }
        if (look->standard < 0) {
            if (name[0] == '_') goto bad;
        } else {
            if (name[0] != '_' && ((p->flags & CONF_UNDERSCORE) || look->section <= 1)) goto bad;
            if (name[0] == '_' && (p->flags & CONF_NOUNDERSCORE)) goto bad;
        }
    }
    s = 0;
    defined = 1;
    i = (p->op < 0 || ((flags & CONF_MINMAX) && (p->flags & CONF_MINMAX_DEF))) ? 0 : p->call;
    switch (i) {
        case CONF_confstr:
            call = "confstr";
            v = confstr(p->op, buf, sizeof(buf));
            if (v == 0) {
                defined = 0;
                v = -1;
                errno = EINVAL;
            } else if (v > 0) {
                buf[sizeof(buf) - 1] = 0;
                s = (const char *)buf;
            } else {
                defined = 0;
            }
            break;
        case CONF_pathconf:
            call = "pathconf";
            v = pathconf(path, p->op);
            if (v == -1) defined = 0;
            break;
        case CONF_sysconf:
            call = "sysconf";
            v = sysconf(p->op);
            if (v == -1) defined = 0;
            break;
        case CONF_sysinfo:
            call = "sysinfo";
#if _lib_sysinfo
            if ((v = sysinfo(p->op, buf, sizeof(buf))) >= 0) {
                buf[sizeof(buf) - 1] = 0;
                s = (const char *)buf;
            } else
                defined = 0;
            break;
#else
            goto predef;
#endif
        default:
            call = "synthesis";
            errno = EINVAL;
            v = -1;
            defined = 0;
            break;
        case 0:
            call = 0;
            if (p->standard == CONF_AST) {
                if (!strcmp(p->name, "RELEASE") &&
                    (i = open("/proc/version", O_RDONLY | O_CLOEXEC)) >= 0) {
                    n = read(i, buf, sizeof(buf) - 1);
                    close(i);
                    if (n > 0 && buf[n - 1] == '\n') n--;
                    if (n > 0 && buf[n - 1] == '\r') n--;
                    buf[n] = 0;
                    if (buf[0]) {
                        v = 0;
                        s = buf;
                        break;
                    }
                }
            }
            if (p->flags & CONF_MINMAX_DEF) {
                if (!((p->flags & CONF_LIMIT_DEF))) flags |= CONF_MINMAX;
                listflags &= ~ASTCONF_system;
            }
#if !_lib_sysinfo
        predef:
#endif
            if (look->standard == CONF_AST) {
                if (!strcmp(p->name, "VERSION")) {
                    v = ast.version;
                    break;
                }
            }
            if (flags & CONF_MINMAX) {
                if ((p->flags & CONF_MINMAX_DEF) &&
                    (!(listflags & ASTCONF_system) || !(p->flags & CONF_DEFER_MM))) {
                    v = p->minmax.number;
                    s = p->minmax.string;
                    break;
                }
            } else if ((p->flags & CONF_LIMIT_DEF) &&
                       (!(listflags & ASTCONF_system) || !(p->flags & CONF_DEFER_CALL))) {
                v = p->limit.number;
                s = p->limit.string;
                break;
            }
            flags &= ~(CONF_LIMIT_DEF | CONF_MINMAX_DEF);
            v = -1;
            errno = EINVAL;
            defined = 0;
            break;
    }
    if (!defined) {
        if (!errno) {
            if ((p->flags & CONF_FEATURE) || !(p->flags & (CONF_LIMIT | CONF_MINMAX))) {
                flags &= ~(CONF_LIMIT_DEF | CONF_MINMAX_DEF);
            }
        } else if (flags & CONF_PREFIXED) {
            flags &= ~(CONF_LIMIT_DEF | CONF_MINMAX_DEF);
        } else if (errno != EINVAL || !i) {
            if (!sp) {
                if (conferror) {
                    if (call) {
                        (*conferror)(&state, &state, ERROR_SYSTEM | 2, "%s: %s error", p->name,
                                     call);
                    } else if (!(listflags & ASTCONF_system)) {
                        (*conferror)(&state, &state, 2, "%s: unknown name", p->name);
                    }
                }
                goto bad;
            } else {
                flags &= ~(CONF_LIMIT_DEF | CONF_MINMAX_DEF);
                flags |= CONF_ERROR;
            }
        }
    }
    errno = olderrno;
    if ((listflags & ASTCONF_defined) && !(flags & (CONF_LIMIT_DEF | CONF_MINMAX_DEF))) goto bad;
    if ((drop = !sp) && !(sp = sfstropen())) goto bad;
    if (listflags & ASTCONF_table) {
        f = flg;
        if (p->flags & CONF_DEFER_CALL) *f++ = 'C';
        if (p->flags & CONF_DEFER_MM) *f++ = 'D';
        if (p->flags & CONF_FEATURE) *f++ = 'F';
        if (p->flags & CONF_LIMIT) *f++ = 'L';
        if (p->flags & CONF_MINMAX) *f++ = 'M';
        if (p->flags & CONF_NOSECTION) *f++ = 'N';
        if (p->flags & CONF_PREFIXED) *f++ = 'P';
        if (p->flags & CONF_STANDARD) *f++ = 'S';
        if (p->flags & CONF_UNDERSCORE) *f++ = 'U';
        if (p->flags & CONF_NOUNDERSCORE) *f++ = 'V';
        if (p->flags & CONF_PREFIX_ONLY) *f++ = 'W';
        if (f == flg) *f++ = 'X';
        *f = 0;
        sfprintf(sp, "%*s %*s %d %2s %4d %6s ", sizeof(p->name), p->name,
                 sizeof(prefix[p->standard].name), prefix[p->standard].name, p->section,
                 prefix[p->call + CONF_call].name, p->op, flg);
        if (p->flags & CONF_LIMIT_DEF) {
            if (p->limit.string) {
                sfprintf(sp, "L[%s] ",
                         (listflags & ASTCONF_quote) ? fmtquote(p->limit.string, "\"", "\"",
                                                                strlen(p->limit.string), FMT_SHELL)
                                                     : p->limit.string);
            } else {
                sfprintf(sp, "L[%I*d] ", sizeof(p->limit.number), p->limit.number);
            }
        }
        if (p->flags & CONF_MINMAX_DEF) {
            if (p->minmax.string) {
                sfprintf(sp, "M[%s] ",
                         (listflags & ASTCONF_quote) ? fmtquote(p->minmax.string, "\"", "\"",
                                                                strlen(p->minmax.string), FMT_SHELL)
                                                     : p->minmax.string);
            } else {
                sfprintf(sp, "M[%I*d] ", sizeof(p->minmax.number), p->minmax.number);
            }
        }
        if (flags & CONF_ERROR) {
            sfprintf(sp, "error");
        } else if (defined) {
            if (s) {
                sfprintf(sp, "%s",
                         (listflags & ASTCONF_quote) ? fmtquote(s, "\"", "\"", strlen(s), FMT_SHELL)
                                                     : s);
            } else if (v != -1) {
                sfprintf(sp, "%I*d", sizeof(v), v);
            } else {
                sfprintf(sp, "%I*u", sizeof(v), v);
            }
        }
        sfprintf(sp, "\n");
    } else {
        if (!(flags & CONF_PREFIXED) || (listflags & ASTCONF_base)) {
            if (!name) {
                if ((p->flags & (CONF_PREFIXED | CONF_STRING)) == (CONF_PREFIXED | CONF_STRING) &&
                    (!(listflags & ASTCONF_base) || p->standard != CONF_POSIX)) {
                    if ((p->flags & CONF_UNDERSCORE) && !(listflags & ASTCONF_base)) {
                        sfprintf(sp, "_");
                    }
                    char *lower = listflags & ASTCONF_lower
                                      ? astconf_fmtlower(prefix[p->standard].name)
                                      : (char *)prefix[p->standard].name;
                    sfprintf(sp, "%s", s);
                    if (listflags & ASTCONF_lower) free(lower);
                    if (p->section > 1) sfprintf(sp, "%d", p->section);
                    sfprintf(sp, "_");
                }
                char *lower =
                    listflags & ASTCONF_lower ? astconf_fmtlower(p->name) : (char *)p->name;
                sfprintf(sp, "%s=", lower);
                if (listflags & ASTCONF_lower) free(lower);
            }
            if (flags & CONF_ERROR) {
                sfprintf(sp, "error");
            } else if (defined) {
                if (s) {
                    sfprintf(sp, "%s",
                             (listflags & ASTCONF_quote)
                                 ? fmtquote(s, "\"", "\"", strlen(s), FMT_SHELL)
                                 : s);
                } else if (v != -1) {
                    sfprintf(sp, "%I*d", sizeof(v), v);
                } else {
                    sfprintf(sp, "%I*u", sizeof(v), v);
                }
            } else {
                sfprintf(sp, "undefined");
            }
            if (!name) sfprintf(sp, "\n");
        }
        if (!name && !(listflags & ASTCONF_base) && !(p->flags & CONF_STRING) &&
            (p->flags & (CONF_FEATURE | CONF_MINMAX))) {
            if (p->flags & CONF_UNDERSCORE) sfprintf(sp, "_");
            char *lower = listflags & ASTCONF_lower ? astconf_fmtlower(prefix[p->standard].name)
                                                    : (char *)prefix[p->standard].name;
            sfprintf(sp, "%s", lower);
            if (listflags & ASTCONF_lower) free(lower);
            if (p->section > 1) sfprintf(sp, "%d", p->section);
            lower = listflags & ASTCONF_lower ? astconf_fmtlower(p->name) : (char *)p->name;
            sfprintf(sp, "_%s=", lower);
            if (listflags & ASTCONF_lower) free(lower);
            if (!defined && !name && (p->flags & CONF_MINMAX_DEF)) {
                defined = 1;
                v = p->minmax.number;
            }
            if (v != -1) {
                sfprintf(sp, "%I*d", sizeof(v), v);
            } else if (defined) {
                sfprintf(sp, "%I*u", sizeof(v), v);
            } else {
                sfprintf(sp, "undefined");
            }
            sfprintf(sp, "\n");
        }
    }
    if (drop) {
        call = sfstruse(sp);
        if (call) {
            call = astconf_buffer(call);
        } else {
            call = "[ out of space ]";
        }
        sfclose(sp);
        return call;
    }
bad:
    if (!(listflags & ~(ASTCONF_error | ASTCONF_system))) {
        for (fp = state.features; fp; fp = fp->next) {
            if (!strcmp(name, fp->name)) {
                return astconf_format(fp, path, NULL, listflags, conferror);
            }
        }
    }
    return (listflags & ASTCONF_error) ? NULL : "";
}

#ifdef _pth_getconf_a
/*
 * return read stream to native getconf utility
 */
static_fn Sfio_t *nativeconf(Proc_t **pp, const char *operand) {
#ifdef _pth_getconf
    Sfio_t *sp;
    char *cmd[3];
    long ops[2];

#if DEBUG_astconf
    error(-6, "astconf defer %s %s", _pth_getconf, operand);
#endif
    cmd[0] = (char *)state.id;
    cmd[1] = (char *)operand;
    cmd[2] = 0;
    ops[0] = PROC_FD_DUP(open("/dev/null", O_WRONLY, 0), 2, PROC_FD_CHILD);
    ops[1] = 0;
    *pp = procopen(_pth_getconf, cmd, environ, ops, PROC_READ);
    if (*pp) {
        sp = sfnew(NULL, NULL, SF_UNBOUND, (*pp)->rfd, SF_READ);
        if (sp) {
            sfdisc(sp, SF_POPDISC);
            return sp;
        }
        procclose(*pp);
    }
#endif
    return 0;
}
#endif  // #ifdef _pth_getconf_a

/*
 * value==0 gets value for name
 * value!=0 sets value for name and returns previous value
 * path==0 implies path=="/"
 *
 * settable return values are in permanent store
 * non-settable return values copied to a tmp fmtbuf() buffer
 *
 *      if (!strcmp(astgetconf("PATH_RESOLVE", NULL, NULL, 0, 0), "logical"))
 *              our_way();
 *
 *      universe = astgetconf("UNIVERSE", NULL, "att", 0, 0);
 *      astgetconf("UNIVERSE", NULL, universe, 0, 0);
 *
 * if (flags&ASTCONF_error)!=0 then error return value is 0
 * otherwise 0 not returned
 */

#define ALT 16

char *astgetconf(const char *name, const char *path, const char *value, int flags,
                 Error_f conferror) {
    char *s;
    int n;
    Lookup_t look;
    Sfio_t *tmp;

    if (!name) {
        if (path) return "";
        if (!(name = value)) {
            if (state.data) {
                Ast_confdisc_f notify;

#if _HUH20000515 /* doesn't work for shell builtins */
                free(state.data - state.prefix);
#endif
                state.data = 0;
                notify = state.notify;
                state.notify = 0;
                INITIALIZE();
                state.notify = notify;
            }
            return "";
        }
        value = 0;
    }
    INITIALIZE();
    if (!path) path = root;
    if (state.recent && !strcmp(name, state.recent->name) &&
        (s = astconf_format(state.recent, path, value, flags, conferror))) {
        return s;
    }
    if (astconf_lookup(&look, name, flags)) {
        if (value) {
        ro:
            errno = EINVAL;
            if (conferror) (*conferror)(&state, &state, 2, "%s: cannot set value", name);
            return (flags & ASTCONF_error) ? NULL : "";
        }
        return astconf_print(NULL, &look, name, path, flags, conferror);
    }
    if ((n = strlen(name)) > 3 && n < (ALT + 3)) {
        if (!strcmp(name + n - 3, "DEV")) {
            tmp = sfstropen();
            if (tmp) {
                sfprintf(tmp, "/dev/");
                for (s = (char *)name; s < (char *)name + n - 3; s++) {
                    sfputc(tmp, isupper(*s) ? tolower(*s) : *s);
                }
                if ((s = sfstruse(tmp)) && !access(s, F_OK)) {
                    if (value) goto ro;
                    s = astconf_buffer(s);
                    sfclose(tmp);
                    return s;
                }
                sfclose(tmp);
            }
        } else if (!strcmp(name + n - 3, "DIR")) {
            Lookup_t altlook;
            char altname[ALT];

            static const char *dirs[] = {"/usr/lib", "/usr", ""};

            strcpy(altname, name);
            altname[n - 3] = 0;
            if (astconf_lookup(&altlook, altname, flags)) {
                if (value) {
                    errno = EINVAL;
                    if (conferror) (*conferror)(&state, &state, 2, "%s: cannot set value", altname);
                    return (flags & ASTCONF_error) ? NULL : "";
                }
                return astconf_print(NULL, &altlook, altname, path, flags, conferror);
            }
            for (s = altname; *s; s++) {
                if (isupper(*s)) *s = tolower(*s);
            }
            tmp = sfstropen();
            if (tmp) {
                for (n = 0; n < elementsof(dirs); n++) {
                    sfprintf(tmp, "%s/%s/.", dirs[n], altname);
                    if ((s = sfstruse(tmp)) && !access(s, F_OK)) {
                        if (value) goto ro;
                        s = astconf_buffer(s);
                        sfclose(tmp);
                        return s;
                    }
                }
                sfclose(tmp);
            }
        }
    }
    if ((look.standard < 0 || look.standard == CONF_AST) && look.call <= 0 && look.section <= 1 &&
        (s = astconf_feature(0, look.name, path, value, flags, conferror))) {
        return s;
    }
    errno = EINVAL;
    if (conferror && !(flags & ASTCONF_system)) {
        (*conferror)(&state, &state, 2, "%s: unknown name", name);
    }
    return (flags & ASTCONF_error) ? NULL : "";
}

/*
 * astconf() never returns 0
 */

char *astconf(const char *name, const char *path, const char *value) {
#if DEBUG_astconf
    char *r;

    r = astgetconf(name, path, value, 0, 0);
    error(-1, "astconf(%s,%s,%s) => '%s'", name, path, value, r);
    return r;
#else
    return astgetconf(name, path, value, 0, 0);
#endif
}

/*
 * set discipline function to be called when features change
 * old discipline function returned
 */

Ast_confdisc_f astconfdisc(Ast_confdisc_f new_notify) {
    Ast_confdisc_f old_notify;

    INITIALIZE();
    old_notify = state.notify;
    state.notify = new_notify;
    return old_notify;
}

/*
 * list all name=value entries on sp
 * path==0 implies path=="/"
 */

void astconflist(Sfio_t *sp, const char *path, int flags, const char *pattern) {
    char *s;
    char *f;
    char *call;
    Feature_t *fp;
    Lookup_t look;
    regex_t re;
    regdisc_t redisc;
    int olderrno;
    char flg[8];
#ifdef _pth_getconf_a
    Proc_t *proc;
    Sfio_t *pp;
#endif

    INITIALIZE();
    if (!path) {
        path = root;
    } else if (access(path, F_OK)) {
        errorf(&state, &state, 2, "%s: not found", path);
        return;
    }
    olderrno = errno;
    look.flags = 0;
    if (!(flags & (ASTCONF_read | ASTCONF_write | ASTCONF_parse))) {
        flags |= ASTCONF_read | ASTCONF_write;
    } else if (flags & ASTCONF_parse) {
        flags |= ASTCONF_write;
    }
    if (!(flags & (ASTCONF_matchcall | ASTCONF_matchname | ASTCONF_matchstandard))) pattern = 0;
    if (pattern) {
        memset(&redisc, 0, sizeof(redisc));
        redisc.re_version = REG_VERSION;
        redisc.re_errorf = (regerror_t)errorf;
        re.re_disc = &redisc;
        if (regcomp(&re, pattern, REG_DISCIPLINE | REG_EXTENDED | REG_LENIENT | REG_NULL)) return;
    }
    if (flags & ASTCONF_read) {
        for (look.conf = (Conf_t *)conf; look.conf < (Conf_t *)&conf[conf_elements]; look.conf++) {
            if (pattern) {
                if (flags & ASTCONF_matchcall) {
                    if (regexec(&re, prefix[look.conf->call + CONF_call].name, 0, NULL, 0)) {
                        continue;
                    }
                } else if (flags & ASTCONF_matchname) {
                    if (regexec(&re, look.conf->name, 0, NULL, 0)) continue;
                } else if (flags & ASTCONF_matchstandard) {
                    if (regexec(&re, prefix[look.conf->standard].name, 0, NULL, 0)) continue;
                }
            }
            look.standard = look.conf->standard;
            look.section = look.conf->section;
            astconf_print(sp, &look, NULL, path, flags, errorf);
        }
#ifdef _pth_getconf_a
        pp = nativeconf(&proc, _pth_getconf_a);
        if (pp) {
            call = "GC";
            while ((f = sfgetr(pp, '\n', 1))) {
                for (s = f; *s && *s != '=' && *s != ':' && !isspace(*s); s++)
                    ;
                if (*s)
                    for (*s++ = 0; isspace(*s); s++)
                        ;
                if (!astconf_lookup(&look, f, flags)) {
                    if (flags & ASTCONF_table) {
                        if (look.standard < 0) look.standard = 0;
                        if (look.section < 1) look.section = 1;
                        sfprintf(sp, "%*s %*s %d %2s %4d %5s %s\n", sizeof(conf[0].name), f,
                                 sizeof(prefix[look.standard].name), prefix[look.standard].name,
                                 look.section, call, 0, "N", s);
                    } else if (flags & ASTCONF_parse)
                        sfprintf(sp, "%s %s - %s\n", state.id, f, s);
                    else
                        sfprintf(sp, "%s=%s\n", f,
                                 (flags & ASTCONF_quote)
                                     ? fmtquote(s, "\"", "\"", strlen(s), FMT_SHELL)
                                     : s);
                }
            }
            sfclose(pp);
            procclose(proc);
        }
#endif
    }
    if (flags & ASTCONF_write) {
        call = "AC";
        for (fp = state.features; fp; fp = fp->next) {
            if (pattern) {
                if (flags & ASTCONF_matchcall) {
                    if (regexec(&re, call, 0, NULL, 0)) continue;
                } else if (flags & ASTCONF_matchname) {
                    if (regexec(&re, fp->name, 0, NULL, 0)) continue;
                } else if (flags & ASTCONF_matchstandard) {
                    if (regexec(&re, prefix[fp->standard].name, 0, NULL, 0)) continue;
                }
            }
            if (!(s = astconf_feature(fp, 0, path, NULL, 0, 0)) || !*s) s = "0";
            if (flags & ASTCONF_table) {
                f = flg;
                if (fp->flags & CONF_ALLOC) *f++ = 'A';
                if (fp->flags & CONF_READONLY) *f++ = 'R';
                if (f == flg) *f++ = 'X';
                *f = 0;
                sfprintf(sp, "%*s %*s %d %2s %4d %5s %s\n", sizeof(conf[0].name), fp->name,
                         sizeof(prefix[fp->standard].name), prefix[fp->standard].name, 1, call, 0,
                         flg, s);
            } else if (flags & ASTCONF_parse) {
                char *lower = flags & ASTCONF_lower ? astconf_fmtlower(fp->name) : (char *)fp->name;
                sfprintf(sp, "%s %s - %s\n", state.id, lower,
                         fmtquote(s, "\"", "\"", strlen(s), FMT_SHELL));
                if (flags & ASTCONF_lower) free(lower);
            } else {
                char *lower = flags & ASTCONF_lower ? astconf_fmtlower(fp->name) : (char *)fp->name;
                sfprintf(
                    sp, "%s=%s\n", lower,
                    (flags & ASTCONF_quote) ? fmtquote(s, "\"", "\"", strlen(s), FMT_SHELL) : s);
                if (flags & ASTCONF_lower) free(lower);
            }
        }
    }
    if (pattern) regfree(&re);
    errno = olderrno;
}
