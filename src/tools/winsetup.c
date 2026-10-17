/*
 * winsetup.c - installer and uninstaller for the WINDISK and WINUSB packages (DOS)
 *
 * One source, four programs:
 *   INSTDISK.EXE / UNINDISK.EXE   (default)
 *   INSTUSB.EXE  / UNINUSB.EXE    (-DWIN_USB)
 *   INSTAUD.EXE  / UNINAUD.EXE    (-DWIN_AUDIO)
 *   INSTUDF.EXE  / UNINUDF.EXE    (-DWIN_UDF)
 * The uninstallers are built with -DUNINST.
 *
 * The installer copies the package to a directory (default: the Windows directory),
 * adds DEVICE=...\Winxxx.SYS to CONFIG.SYS after the memory managers (WINUSB
 * after WINDISK) and, for WINUSB, sets up WINMOUSE.DRV and WINWHEEL.EXE in
 * SYSTEM.INI / WIN.INI; for WINAUDIO, WINSOUND.DRV in SYSTEM.INI [drivers] and the
 * "General MIDI" setup of the MIDI Mapper on the WIN synthesizer. WINUDF goes to
 * AUTOEXEC.BAT after MSCDEX (DOS, Windows 1.x-3.x; DOSSTART.BAT for the MS-DOS mode of
 * Windows 95/98) and loads WINUDF.VXD in Windows 95/98/ME. The programs get a Program
 * Manager group (Windows 3.x) or a Start Menu group (Windows 95/98/ME). Every edited file is
 * saved first as *.WND, *.WNU or *.WNA.
 * The uninstaller reverses all of it. Both refuse to run inside Windows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dos.h>
#include <direct.h>
#include <io.h>

#ifdef WIN_USB
#define PKG      "WINUSB"
#define SYSFILE  "WINUSB.SYS"
#define OTHERSYS "WINDISK.SYS"
#define BAKEXT   "WNU"
#define INSTEXE  "INSTUSB.EXE"
#define UNINEXE  "UNINUSB.EXE"
#elif defined(WIN_AUDIO)
#define PKG      "WINAUDIO"
#define SYSFILE  "WINAUDIO.SYS"
#define OTHERSYS "WINDISK.SYS"
#define BAKEXT   "WNA"
#define INSTEXE  "INSTAUD.EXE"
#define UNINEXE  "UNINAUD.EXE"
#elif defined(WIN_UDF)
#define PKG      "WINUDF"
#define SYSFILE  "WINUDF.EXE"
#define OTHERSYS "WINDISK.SYS"
#define BAKEXT   "WNF"
#define INSTEXE  "INSTUDF.EXE"
#define UNINEXE  "UNINUDF.EXE"
#else
#define WIN_DISKPKG
#define PKG      "WINDISK"
#define SYSFILE  "WINDISK.SYS"
#define OTHERSYS "WINUSB.SYS"
#define BAKEXT   "WND"
#define INSTEXE  "INSTDISK.EXE"
#define UNINEXE  "UNINDISK.EXE"
#endif

#define MOUSEDESC "WINDRV USB + PS/2 mouse with wheel"
#define DEFMOUSE  "mouse.drv"
#define DEFDESC   "Microsoft, or IBM PS/2"

#define MAXL  4000          /* lines per text file */
#define LMAX  2048          /* characters per line */
#define VMAX  1024          /* INI value buffer */
#define PMAX  128           /* path buffer */

/* package files; 'need' marks the ones the installer cannot do without, 'pkgs' the
   packages that ship the file (it stays while another of them is installed there) */
#define P_DISK  1
#define P_USB   2
#define P_AUD   4
#define P_UDF   8
#ifdef WIN_USB
#define P_SELF  P_USB
#elif defined(WIN_AUDIO)
#define P_SELF  P_AUD
#elif defined(WIN_UDF)
#define P_SELF  P_UDF
#else
#define P_SELF  P_DISK
#endif
static const struct { const char *name; int need, pkgs; } pkgfiles[] = {
#ifdef WIN_USB
    { "WINUSB.SYS", 1, P_USB }, { "WINUCORE.BIN", 1, P_USB }, { "WINUSB.386", 1, P_USB },
    { "WINMOUSE.DRV", 1, P_USB }, { "WINWHEEL.EXE", 1, P_USB }, { "USBTREE.EXE", 0, P_USB },
    { "WINCAM.EXE", 0, P_USB }, { "WINUCD.SYS", 0, P_USB },
    { "WINUSB.VXD", 0, P_USB }, { "WINUSB.PDR", 0, P_USB }, { "WINUSB.INF", 0, P_USB },
    { "WINUSBD.INF", 0, P_USB }, { "WINUSBH.INF", 0, P_USB }, { "WINUSBA.INF", 0, P_USB },
    { "WINUSBV.INF", 0, P_USB }, { "WINUSBS.INF", 0, P_USB }, { "WINKBD.INF", 0, P_USB },
    { "WINMOU.INF", 0, P_USB }, { "WINDRVCI.DLL", 0, P_USB },
    { "OEMSETUP.INF", 0, P_USB }, { "WINSOUND.DRV", 0, P_USB | P_AUD }, { "WINMIX.EXE", 0, P_USB | P_AUD },
    { "AUDTEST.EXE", 0, P_USB | P_AUD }, { "WINBEEP.DRV", 0, P_USB | P_AUD },
#elif defined(WIN_AUDIO)
    { "WINAUDIO.SYS", 1, P_AUD }, { "WINACORE.BIN", 1, P_AUD }, { "WINAUDIO.386", 1, P_AUD },
    { "WINAUDIO.VXD", 0, P_AUD }, { "WINAUDIO.INF", 0, P_AUD }, { "WINAUDM.INF", 0, P_AUD },
    { "WINSOUND.DRV", 1, P_USB | P_AUD }, { "AUDTEST.EXE", 0, P_USB | P_AUD }, { "WINMIX.EXE", 0, P_USB | P_AUD },
    { "WINBEEP.DRV", 0, P_USB | P_AUD },
#elif defined(WIN_UDF)
    { "WINUDF.EXE", 1, P_UDF }, { "WINUDF.VXD", 0, P_UDF },
#else
    { "WINDISK.SYS", 1, P_DISK }, { "WINDCORE.BIN", 1, P_DISK }, { "WINDISK.386", 1, P_DISK },
    { "WINCD.SYS", 0, P_DISK }, { "WINDISK.VXD", 0, P_DISK }, { "WINDISK.PDR", 0, P_DISK },
    { "WINDISK.INF", 0, P_DISK }, { "WINDISKD.INF", 0, P_DISK }, { "WINDISKC.INF", 0, P_DISK },
#endif
#ifndef WIN_UDF
    { "WIN4G.VXD", 0, P_DISK | P_USB | P_AUD },
#endif
#if !defined(WIN_AUDIO) && !defined(WIN_UDF)
    { "WININFO.EXE", 0, P_DISK | P_USB }, { "DSKBENCH.EXE", 0, P_DISK | P_USB },
#endif
#ifndef WIN_UDF
    { "WIN30FIX.COM", 0, P_DISK | P_USB | P_AUD },
#endif
    { "WINGRP.EXE", 0, P_DISK | P_USB | P_AUD | P_UDF },
    { PKG ".TXT", 0, P_SELF },
    { INSTEXE, 0, P_SELF }, { UNINEXE, 1, P_SELF },
    { NULL, 0, 0 }
};

typedef struct {
    char *l[MAXL];
    int n;
} TEXT;

static char bootdrv = 'C';
static int warnings;

#define SP(c) isspace((unsigned char)(c))

/* ---------------------------------------------------------------- strings */

static char *skipws(const char *s)
{
    while (*s && SP(*s))
        s++;
    return (char *)s;
}

static void rtrim(char *s)
{
    int n = strlen(s);
    while (n && SP(s[n - 1]))
        s[--n] = 0;
}

static const char *base(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    return b;
}

static void strip_slash(char *p)
{
    int n = strlen(p);
    if (n > 3 && p[n - 1] == '\\')
        p[n - 1] = 0;
}

static void join(char *out, const char *dir, const char *name)
{
    int n = strlen(dir);
    if (n && dir[n - 1] == '\\')
        sprintf(out, "%s%s", dir, name);
    else
        sprintf(out, "%s\\%s", dir, name);
}

static void dir_of(char *out, const char *path)
{
    int n;
    strcpy(out, path);
    n = base(out) - out;
    out[n] = 0;
    if (n > 3 && out[n - 1] == '\\')
        out[n - 1] = 0;
}


#if !defined(WIN_UDF) || defined(UNINST)
static void warn(const char *msg, const char *arg)
{
    printf("  WARNING: ");
    printf(msg, arg);
    printf("\n");
    warnings++;
}
#endif

/* ---------------------------------------------------------------- DOS */

static int exists(const char *path)
{
    unsigned a;
    return _dos_getfileattr(path, &a) == 0 && !(a & _A_SUBDIR);
}

static int isdir(const char *path)
{
    unsigned a;
    if (strlen(path) == 2 && path[1] == ':')
        return 1;
    if (strlen(path) == 3 && path[2] == '\\')
        return 1;
    return _dos_getfileattr(path, &a) == 0 && (a & _A_SUBDIR);
}

#ifndef UNINST
/* a file of the package: in the installer's directory, else in the directories of the other
   systems next to it (the package's bin\DOS, bin\WIN3X and bin\WIN9X) */
static void pkg_file(char *out, const char *src, const char *name)
{
    static const char *const sib[] = { "DOS", "WIN3X", "WIN9X" };
    char up[PMAX], d[PMAX];
    int i;
    join(out, src, name);
    if (exists(out) || isdir(out))
        return;
    dir_of(up, src);
    if (!stricmp(up, src))
        return;
    for (i = 0; i < 3; i++) {
        join(d, up, sib[i]);
        join(out, d, name);
        if (exists(out) || isdir(out))
            return;
    }
    join(out, src, name);
}
#endif

/* canonical upper-case full path (INT 21h AH=60h); the path need not exist */
static int truename(const char *in, char *out)
{
    union REGS r;
    struct SREGS s;
    char buf[PMAX];
    segread(&s);
    r.h.ah = 0x60;
    s.ds = FP_SEG((void __far *)in);
    r.x.si = FP_OFF((void __far *)in);
    s.es = FP_SEG((void __far *)buf);
    r.x.di = FP_OFF((void __far *)buf);
    intdosx(&r, &r, &s);
    if (r.x.cflag)
        return -1;
    strcpy(out, buf);
    strip_slash(out);
    return 0;
}

/* writes the dirty DOS buffers and the SMARTDRV cache, so that the changes survive a
   reset or power-off right after setup */
static void flush_disks(void)
{
    union REGS r;
    r.x.ax = 0x4A10;
    r.x.bx = 0x0001;
    int86(0x2F, &r, &r);
    r.h.ah = 0x0D;
    int86(0x21, &r, &r);
}

static char boot_drive(void)
{
    union REGS r;
    r.x.ax = 0x3305;
    intdos(&r, &r);
    if (r.h.al == 0xFF || r.h.dl < 1 || r.h.dl > 26)
        return 'C';
    return (char)('A' + r.h.dl - 1);
}

/* Windows (enhanced, standard or real mode) or a task switcher is running */
static int win9x_box;           /* running in a DOS box of Windows 95/98/ME */
#ifndef UNINST
static int dosdrv;              /* /DOS: the DOS driver in CONFIG.SYS of MS-DOS 7 too */
#endif

/* critical errors (sharing violation on a driver file Windows has open) fail the call
   instead of asking Abort, Retry, Fail */
static int __far critical_error(unsigned deverr, unsigned errcode, unsigned __far *devhdr)
{
    (void)deverr; (void)errcode; (void)devhdr;
    _hardresume(_HARDERR_FAIL);
    return _HARDERR_FAIL;
}

static int windows_running(void)
{
    union REGS r;
    r.x.ax = 0x1600;
    int86(0x2F, &r, &r);
    if (r.h.al != 0x00 && r.h.al != 0x80) {
        /* Windows 95/98/ME: the files only get copied, the drivers are installed from Device
           Manager; a DOS box is good enough (Windows ME has nothing else) */
        if (r.h.al >= 4 && r.h.al < 0x80) {
            win9x_box = 1;
            return 0;
        }
        return 1;
    }
    r.x.ax = 0x4680;
    int86(0x2F, &r, &r);
    return r.x.ax == 0;
}

#if !defined(UNINST)
/* MS-DOS 7.x: the DOS of Windows 95/98 (Windows ME has no CONFIG.SYS at all) */
static int dos7(void)
{
    union REGS r;
    r.h.ah = 0x30;
    r.h.al = 0;
    int86(0x21, &r, &r);
    return r.h.al >= 7;
}
#endif

#if !defined(UNINST)
static int make_dir(const char *d)
{
    char tmp[PMAX];
    char *p;
    strcpy(tmp, d);
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            mkdir(tmp);
            *p = '\\';
        }
    }
    mkdir(tmp);
    return isdir(tmp) ? 0 : -1;
}

#endif
static int copy_file(const char *src, const char *dst)
{
    static char buf[8192];
    FILE *i, *o;
    size_t n;
    unsigned d, t;
    int bad = 0;

    i = fopen(src, "rb");
    if (!i)
        return -1;
    _dos_setfileattr(dst, _A_NORMAL);
    o = fopen(dst, "wb");
    if (!o) {
        fclose(i);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), i)) > 0)
        if (fwrite(buf, 1, n, o) != n) {
            bad = 1;
            break;
        }
    if (ferror(i))
        bad = 1;
    if (fflush(o))
        bad = 1;
    if (!_dos_getftime(fileno(i), &d, &t))
        _dos_setftime(fileno(o), d, t);
    fclose(i);
    if (fclose(o))
        bad = 1;
    if (bad)
        remove(dst);
    return bad ? -1 : 0;
}

#if defined(UNINST)
static int del_file(const char *path)
{
    if (!exists(path))
        return 1;
    _dos_setfileattr(path, _A_NORMAL);
    return remove(path) ? -1 : 0;
}

#if defined(WIN_AUDIO) || defined(WIN_USB)
/* size of a file, -1 if it does not exist */
static long file_size(const char *path)
{
    struct find_t ff;
    if (_dos_findfirst(path, _A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH, &ff))
        return -1;
    return (long)ff.size;
}
#endif
#endif

#ifdef WIN_AUDIO
/* the firmware of the Intel audio DSPs: the WINDRVFW directory of the package, next to the
   DOS driver and in WINDOWS\SYSTEM (Windows 95/98/ME) */
#define FWDIR "WINDRVFW"
#define FWATTR (_A_NORMAL | _A_RDONLY | _A_ARCH)
#if !defined(UNINST)
static int make_dir(const char *d);
/* every file of src\WINDRVFW to dst\WINDRVFW: the number copied, -1 on error */
static int fw_copy(const char *src, const char *dst)
{
    char from[PMAX], to[PMAX], a[PMAX], b[PMAX];
    struct find_t ff;
    int n = 0;
    pkg_file(from, src, FWDIR);
    join(to, dst, FWDIR);
    join(a, from, "*.*");
    if (_dos_findfirst(a, FWATTR, &ff))
        return 0;
    if (make_dir(to)) {
        printf("  Cannot create %s\n", to);
        return -1;
    }
    do {
        join(a, from, ff.name);
        join(b, to, ff.name);
        if (copy_file(a, b)) {
            printf("  Cannot copy %s to %s\n", a, b);
            return -1;
        }
        n++;
    } while (!_dos_findnext(&ff));
    return n;
}
#else
static int del_file(const char *path);
/* the files of dst\WINDRVFW and the directory: the number deleted */
static int fw_remove(const char *dst)
{
    char dir[PMAX], f[PMAX];
    struct find_t ff;
    int n = 0;
    join(dir, dst, FWDIR);
    join(f, dir, "*.*");
    if (!_dos_findfirst(f, FWATTR, &ff)) {
        do {
            join(f, dir, ff.name);
            if (del_file(f) == 0)
                n++;
        } while (!_dos_findnext(&ff));
    }
    rmdir(dir);
    return n;
}
#endif
#endif
/* ---------------------------------------------------------------- text files */

static TEXT *t_new(void)
{
    TEXT *t = calloc(1, sizeof(TEXT));
    if (!t) {
        printf("Out of memory.\n");
        exit(3);
    }
    return t;
}

static void t_free(TEXT *t)
{
    int i;
    for (i = 0; i < t->n; i++)
        free(t->l[i]);
    free(t);
}

static char *dupstr(const char *s)
{
    char *p = malloc(strlen(s) + 1);
    if (!p) {
        printf("Out of memory.\n");
        exit(3);
    }
    return strcpy(p, s);
}

static int t_ins(TEXT *t, int at, const char *s)
{
    if (t->n >= MAXL)
        return -1;
    memmove(&t->l[at + 1], &t->l[at], (t->n - at) * sizeof(char *));
    t->l[at] = dupstr(s);
    t->n++;
    return 0;
}

static void t_del(TEXT *t, int at)
{
    free(t->l[at]);
    memmove(&t->l[at], &t->l[at + 1], (t->n - at - 1) * sizeof(char *));
    t->n--;
}

static void t_set(TEXT *t, int at, const char *s)
{
    free(t->l[at]);
    t->l[at] = dupstr(s);
}

/* 0 = loaded, 1 = file does not exist, -1 = error */
static int t_load(TEXT *t, const char *path)
{
    static char line[LMAX];
    FILE *f;
    int c, n = 0;

    f = fopen(path, "rb");
    if (!f)
        return exists(path) ? -1 : 1;
    for (;;) {
        c = getc(f);
        if (c == EOF || c == 0x1A || c == '\n') {
            if (c == '\n' || n) {
                line[n] = 0;
                if (t->n >= MAXL) {
                    fclose(f);
                    return -1;
                }
                t->l[t->n++] = dupstr(line);
            }
            n = 0;
            if (c != '\n')
                break;
        } else if (c != '\r') {
            if (n >= LMAX - 1) {
                fclose(f);
                return -1;
            }
            line[n++] = (char)c;
        }
    }
    fclose(f);
    return 0;
}

static void with_ext(char *out, const char *path, const char *ext)
{
    char *dot;
    strcpy(out, path);
    dot = strrchr(out, '.');
    if (!dot || dot < base(out))
        dot = out + strlen(out);
    sprintf(dot, ".%s", ext);
}

/* writes path via a temporary file; the original is kept once as .WND or .WNU */
static int t_save(TEXT *t, const char *path)
{
    char tmp[PMAX], bak[PMAX];
    unsigned attr = _A_NORMAL;
    FILE *f;
    int i, bad = 0, had;

    with_ext(tmp, path, "$$$");
    with_ext(bak, path, BAKEXT);
    had = exists(path);
    if (had) {
        _dos_getfileattr(path, &attr);
        /* the first backup is the original file; later runs keep it */
        if (!exists(bak) && copy_file(path, bak)) {
            printf("  Cannot write backup %s\n", bak);
            return -1;
        }
    }
    f = fopen(tmp, "wb");
    if (!f) {
        printf("  Cannot create %s\n", tmp);
        return -1;
    }
    for (i = 0; i < t->n; i++)
        if (fputs(t->l[i], f) == EOF || fputs("\r\n", f) == EOF)
            bad = 1;
    if (fclose(f))
        bad = 1;
    if (bad) {
        remove(tmp);
        printf("  Cannot write %s (disk full?)\n", tmp);
        return -1;
    }
    if (had) {
        _dos_setfileattr(path, _A_NORMAL);
        if (remove(path)) {
            remove(tmp);
            printf("  Cannot replace %s\n", path);
            return -1;
        }
    }
    if (rename(tmp, path)) {
        printf("  Cannot rename %s; the old file is in %s\n", tmp, bak);
        return -1;
    }
    _dos_setfileattr(path, attr & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH));
    if (had)
        printf("  %s updated (original: %s)\n", path, bak);
    else
        printf("  %s created\n", path);
    return 0;
}

/* ---------------------------------------------------------------- CONFIG.SYS */

/*
 * 1 if the line loads driver 'name' with DEVICE= or DEVICEHIGH=. The path of
 * the driver and the options after it are returned when the buffers are given.
 */
#ifndef WIN_UDF
static int dev_line(const char *ln, const char *name, char *path, char *opts)
{
    char key[16];
    char tok[PMAX];
    const char *p = skipws(ln);
    const char *s;
    int k = 0, n;

    while (*p && *p != '=' && !SP(*p) && k < 15)
        key[k++] = (char)toupper((unsigned char)*p++);
    key[k] = 0;
    if (k && key[k - 1] == '?')
        key[--k] = 0;
    if (strcmp(key, "DEVICE") && strcmp(key, "DEVICEHIGH"))
        return 0;
    p = skipws(p);
    if (*p != '=')
        return 0;
    p++;
    for (;;) {
        p = skipws(p);
        if (!*p)
            return 0;
        s = p;
        while (*p && !SP(*p))
            p++;
        n = p - s;
        if (*s == '/' || memchr(s, '=', n))
            continue;       /* DEVICEHIGH /L:... or SIZE=... */
        if (n >= PMAX)
            return 0;
        memcpy(tok, s, n);
        tok[n] = 0;
        if (stricmp(base(tok), name))
            return 0;
        if (path)
            strcpy(path, tok);
        if (opts) {
            strncpy(opts, skipws(p), 255);
            opts[255] = 0;
            rtrim(opts);
        }
        return 1;
    }
}
#endif

static int is_section(const char *ln)
{
    return *skipws(ln) == '[';
}

#ifndef UNINST
#ifndef WIN_UDF
/* drivers that WINDISK/WINUSB must follow: XMS and memory managers (and WINDISK for WINUSB) */
static int is_anchor(const char *ln)
{
    static const char *const after[] = {
        "HIMEM.SYS", "HIMEMX.EXE", "HIMEMX.SYS", "EMM386.EXE", "JEMM386.EXE",
        "JEMMEX.EXE", "QEMM386.SYS", "386MAX.SYS",
#ifdef WIN_USB
        "WINDISK.SYS",
#endif
        NULL
    };
    int i;
    for (i = 0; after[i]; i++)
        if (dev_line(ln, after[i], NULL, NULL))
            return 1;
    return 0;
}

static int cfg_install(const char *cfg, const char *dir, const char *uopts)
{
    TEXT *t = t_new();
    char opts[256] = "";
    char o[256];
    char line[PMAX + 300];
    int i, j, e, a, ins, old = 0, added = 0, r;

    printf("Updating %s\n", cfg);
    r = t_load(t, cfg);
    if (r < 0) {
        printf("  Cannot read %s\n", cfg);
        t_free(t);
        return -1;
    }
    for (i = 0; i < t->n;) {
        if (dev_line(t->l[i], SYSFILE, NULL, o)) {
            if (!old)
                strcpy(opts, o);
            old++;
            t_del(t, i);
        } else
            i++;
    }
    if (uopts)
        strcpy(opts, uopts);
    sprintf(line, "DEVICE=%s\\%s%s%s", dir, SYSFILE, *opts ? " " : "", opts);
    if (old)
        printf("  replacing the existing %s line%s\n", SYSFILE, old > 1 ? "s" : "");

    /* every block of a multi-configuration CONFIG.SYS gets its own line */
    for (i = 0; i < t->n; i = e) {
        e = i + 1;
        while (e < t->n && !is_section(t->l[e]))
            e++;
        a = -1;
        for (j = i; j < e; j++)
            if (is_anchor(t->l[j]))
                a = j;
        if (a < 0)
            continue;
        ins = a + 1;
#ifdef WIN_DISKPKG
        for (j = i; j < ins; j++)
            if (dev_line(t->l[j], OTHERSYS, NULL, NULL)) {
                ins = j;
                break;
            }
#endif
        if (t_ins(t, ins, line)) {
            printf("  %s is too long\n", cfg);
            t_free(t);
            return -1;
        }
        added++;
        e++;
    }
    if (!added) {
        /* MS-DOS 7 loads HIMEM.SYS by itself, but only after the CONFIG.SYS drivers */
        const char *wb = getenv("winbootdir");
        char hm[PMAX + 16];
        if (dos7() && wb && strlen(wb) < PMAX) {
            sprintf(hm, "%s\\HIMEM.SYS", wb);
            if (exists(hm)) {
                sprintf(hm, "DEVICE=%s\\HIMEM.SYS", wb);
                t_ins(t, 0, hm);
                printf("  %s (before %s)\n", hm, SYSFILE);
                t_ins(t, 1, line);
                added++;
            }
        }
        if (!added) {
            warn("no HIMEM.SYS line found; %s needs an XMS driver loaded before it", SYSFILE);
            t_ins(t, t->n, line);
        }
    }
    printf("  %s\n", line);
    r = t_save(t, cfg);
    t_free(t);
    return r;
}
#endif
#else
#ifndef WIN_UDF
/* removes every line that loads the driver; returns its directory in dir */
static int cfg_remove(const char *cfg, char *dir)
{
    TEXT *t = t_new();
    char path[PMAX];
    int i, n = 0, r;

    printf("Updating %s\n", cfg);
    r = t_load(t, cfg);
    if (r < 0) {                /* no CONFIG.SYS at all (1) is fine */
        printf("  Cannot read %s\n", cfg);
        t_free(t);
        return -1;
    }
    for (i = 0; i < t->n;) {
        if (dev_line(t->l[i], SYSFILE, path, NULL)) {
            printf("  removing: %s\n", t->l[i]);
            if (!n && !dir[0]) {
                char full[PMAX];
                if (truename(path, full))
                    strcpy(full, path);
                dir_of(dir, full);
            }
            n++;
            t_del(t, i);
        } else
            i++;
    }
    if (!n) {
        printf("  %s is not loaded from %s\n", SYSFILE, cfg);
        t_free(t);
        return 0;
    }
    r = t_save(t, cfg);
    t_free(t);
    return r;
}
#endif
#endif

/* ---------------------------------------------------------------- Windows installations */

/* Windows 95/98/ME: what goes where. 11 = SYSTEM, 12 = SYSTEM\IOSUBSYS, 17 = INF */
struct w9xfile { const char *name; int where; };
static const struct w9xfile w9xfiles[] = {
#ifdef WIN_USB
    { "WINUSB.VXD", 11 }, { "WINUSB.PDR", 12 }, { "WINMOUSE.DRV", 11 }, { "WINSOUND.DRV", 11 },
    { "WINWHEEL.EXE", 11 }, { "USBTREE.EXE", 11 }, { "WINCAM.EXE", 11 }, { "WINMIX.EXE", 11 },
    { "WINDRVCI.DLL", 11 }, { "WINUSB.INF", 17 }, { "WINUSBD.INF", 17 }, { "WINUSBH.INF", 17 },
    { "WINUSBA.INF", 17 }, { "WINUSBV.INF", 17 }, { "WINUSBS.INF", 17 }, { "WINKBD.INF", 17 },
    { "WINMOU.INF", 17 },
#elif defined(WIN_AUDIO)
    { "WINAUDIO.VXD", 11 }, { "WINSOUND.DRV", 11 }, { "WINMIX.EXE", 11 }, { "AUDTEST.EXE", 11 },
    { "WINAUDIO.INF", 17 }, { "WINAUDM.INF", 17 },
#elif defined(WIN_UDF)
    { "WINUDF.VXD", 11 },
#else
    { "WINDISK.VXD", 11 }, { "WINDISK.PDR", 12 }, { "WINDISK.INF", 17 }, { "WINDISKD.INF", 17 },
    { "WINDISKC.INF", 17 },
#endif
#ifndef WIN_UDF
    { "WIN4G.VXD", 11 },
#endif
    { NULL, 0 }
};

static void w9x_path(char *out, const char *win, int where, const char *name)
{
    sprintf(out, "%s\\%s\\%s", win, where == 12 ? "SYSTEM\\IOSUBSYS" : where == 17 ? "INF" : "SYSTEM", name);
}

#ifdef UNINST
/* 1 if a WINDRV package other than this one has its Windows 95/98/ME VxD installed: the
   files they share (WIN4G.VXD, WINSOUND.DRV, WINMIX.EXE) stay for it */
static int w9x_other(const char *win, int pkgs)
{
    static const struct { const char *vxd; int pkg; } v[] = {
        { "WINDISK.VXD", P_DISK }, { "WINUSB.VXD", P_USB }, { "WINAUDIO.VXD", P_AUD }
    };
    char f[PMAX];
    int i;
    for (i = 0; i < 3; i++) {
        if (v[i].pkg == P_SELF || !(v[i].pkg & pkgs))
            continue;
        w9x_path(f, win, 11, v[i].vxd);
        if (exists(f))
            return 1;
    }
    return 0;
}
#endif

#define WV_1X   0x0100      /* Windows 1.0x */
#define WV_2X   0x0200      /* Windows 2.x */
#define WV_386  0x0201      /* Windows/386 2.x */
#define WV_30   0x0300
#define WV_31   0x030A
#define WV_95   0x0400      /* Windows 95, 98, ME: Plug and Play, drivers come from the INF files */
#define WV_98   0x040A
#define WV_ME   0x045A
#define MAXWIN  16
#define IS9X(w) ((w)->ver >= WV_95)

struct winst {
    char dir[PMAX];
    unsigned ver;
    char mme;               /* 3.0 with Multimedia Extensions */
    char wfw;               /* Windows for Workgroups 3.11 */
};

static struct winst wins[MAXWIN];
static int nwins;

static int in_dir(const char *dir, const char *name)
{
    char f[PMAX];
    if (strlen(dir) + strlen(name) + 2 > PMAX)
        return 0;
    join(f, dir, name);
    return exists(f);
}

/* NE header field "expected Windows version" of an executable, 0 if unknown */
static unsigned ne_version(const char *path)
{
    unsigned char h[0x40];
    unsigned ne, v = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fread(h, 1, 0x40, fp) == 0x40 && h[0] == 'M' && h[1] == 'Z') {
        ne = h[0x3C] | (h[0x3D] << 8);
        if (fseek(fp, ne, SEEK_SET) == 0 && fread(h, 1, 0x40, fp) == 0x40 && h[0] == 'N' && h[1] == 'E')
            v = h[0x3E] | (h[0x3F] << 8);
    }
    fclose(fp);
    return v;
}

/* the Windows version installed in 'dir'; 0 = none */
static int win_probe(const char *dir, struct winst *w)
{
    char f[PMAX];
    unsigned v;
    memset(w, 0, sizeof(*w));
    if (!in_dir(dir, "WIN.INI"))
        return 0;
    if (in_dir(dir, "SYSTEM.INI") && in_dir(dir, "WIN.COM")) {
        if (strlen(dir) + 20 > PMAX)
            return 0;
        sprintf(f, "%s\\SYSTEM\\USER.EXE", dir);
        v = ne_version(f);
        if (v == 0x0300) {
            w->ver = WV_30;
            sprintf(f, "%s\\SYSTEM\\MMSYSTEM.DLL", dir);
            w->mme = (char)exists(f);
        } else if (v >= 0x030A && v < 0x0400) {
            w->ver = WV_31;
            sprintf(f, "%s\\SYSTEM\\IFSMGR.386", dir);
            w->wfw = (char)exists(f);
        } else if (v >= 0x0400 && v < 0x040A) {
            w->ver = WV_95;
        } else if (v >= 0x040A && v < 0x045A) {
            w->ver = WV_98;
        } else if (v >= 0x045A && v < 0x0500) {
            w->ver = WV_ME;
        } else {
            return 0;
        }
    } else if (in_dir(dir, "WIN200.BIN")) {
        w->ver = in_dir(dir, "WIN386.EXE") ? WV_386 : WV_2X;
    } else if (in_dir(dir, "WIN100.BIN")) {
        w->ver = WV_1X;
    } else {
        return 0;
    }
    if (truename(dir, w->dir))
        strcpy(w->dir, dir);
    return w->ver;
}

static const char *win_name(const struct winst *w)
{
    switch (w->ver) {
    case WV_1X:  return "Windows 1.0x";
    case WV_2X:  return "Windows 2.x";
    case WV_386: return "Windows/386 2.x";
    case WV_30:  return w->mme ? "Windows 3.0 with Multimedia Extensions" : "Windows 3.0";
    case WV_31:  return w->wfw ? "Windows for Workgroups 3.11" : "Windows 3.1";
    case WV_95:  return "Windows 95";
    case WV_98:  return "Windows 98";
    case WV_ME:  return "Windows ME";
    }
    return "Windows";
}

#ifndef UNINST
/* what this package does for a Windows version; 0 = not supported */
static int pkg_supports(const struct winst *w)
{
#ifdef WIN_USB
    return w->ver != 0;
#elif defined(WIN_AUDIO)
    return w->ver == WV_31 || (w->ver == WV_30 && w->mme) || IS9X(w);
#elif defined(WIN_UDF)
    return w->ver != 0;     /* 1.x-3.x: the DOS redirector; 95/98/ME: WINUDF.VXD */
#else
    /* disks: 3.x get FastDisk / *int13; 1.x/2.x are listed too and told the DOS driver covers them */
    return w->ver != 0;
#endif
}

#endif

static void win_add(const char *dir)
{
    struct winst w;
    int i;
    if (nwins >= MAXWIN || !win_probe(dir, &w))
        return;
    for (i = 0; i < nwins; i++)
        if (!stricmp(wins[i].dir, w.dir))
            return;
    wins[nwins++] = w;
}

/* directories up to two levels below the root of each local hard disk, and the PATH */
static void win_scan(void)
{
    struct find_t f1, f2;
    char d1[PMAX], d2[PMAX], pat[PMAX];
    const char *env = getenv("PATH"), *s;
    int drv, n;
    union REGS r;

    /* a DOS box of Windows 95/98/ME: that Windows, no drive scan (floppy units of the real
       mode mapper and network drives would be touched) */
    if (win9x_box) {
        s = getenv("windir");
        if (s && strlen(s) < PMAX) {
            strcpy(d1, s);
            strip_slash(d1);
            win_add(d1);
        }
        return;
    }
    for (drv = 3; drv <= 26; drv++) {
        r.x.ax = 0x4408;                    /* removable? (also tells if the drive exists) */
        r.h.bl = (unsigned char)drv;
        intdos(&r, &r);
        if (r.x.cflag || r.x.ax != 1)
            continue;
        r.x.ax = 0x4409;                    /* remote (network, CD-ROM) drives are skipped */
        r.h.bl = (unsigned char)drv;
        intdos(&r, &r);
        if (!r.x.cflag && (r.x.dx & 0x1000))
            continue;
        sprintf(d1, "%c:\\", 'A' + drv - 1);
        win_add(d1);
        sprintf(pat, "%c:\\*.*", 'A' + drv - 1);
        if (_dos_findfirst(pat, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM, &f1))
            continue;
        do {
            if (!(f1.attrib & _A_SUBDIR) || f1.name[0] == '.')
                continue;
            sprintf(d1, "%c:\\%s", 'A' + drv - 1, f1.name);
            win_add(d1);
            sprintf(pat, "%s\\*.*", d1);
            if (_dos_findfirst(pat, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM, &f2))
                continue;
            do {
                if (!(f2.attrib & _A_SUBDIR) || f2.name[0] == '.')
                    continue;
                sprintf(d2, "%s\\%s", d1, f2.name);
                win_add(d2);
            } while (!_dos_findnext(&f2));
        } while (!_dos_findnext(&f1));
    }
    while (env && *env) {
        s = env;
        while (*env && *env != ';')
            env++;
        n = env - s;
        if (*env)
            env++;
        if (n <= 0 || n >= PMAX - 16)
            continue;
        memcpy(d1, s, n);
        d1[n] = 0;
        rtrim(d1);
        strip_slash(d1);
        win_add(d1);
    }
}

#ifndef UNINST
/* /W:dir or the scan: fills wins[] with the supported installations; -1 on error */
static int win_select(const char *given, int verbose)
{
    struct winst w;
    int i, k = 0;
    if (given[0]) {
        if (!win_probe(given, &w)) {
            printf("No Windows installation in %s\n", given);
            return -1;
        }
        if (!pkg_supports(&w)) {
            printf("%s in %s is not supported by " PKG "\n", win_name(&w), w.dir);
            return -1;
        }
        wins[0] = w;
        nwins = 1;
    } else {
        win_scan();
        for (i = 0; i < nwins; i++)
            if (pkg_supports(&wins[i]))
                wins[k++] = wins[i];
        nwins = k;
    }
    if (verbose && nwins) {
        printf("Windows installations:\n");
        for (i = 0; i < nwins; i++)
            printf("  %-24s %s\n", wins[i].dir, win_name(&wins[i]));
        printf("\n");
    }
    return 0;
}

#endif

/* ---------------------------------------------------------------- INI files */

/* index of the [sec] header, -1 if missing */
static int ini_sec(TEXT *t, const char *sec)
{
    char name[64];
    int i, n;
    const char *p, *q;
    for (i = 0; i < t->n; i++) {
        p = skipws(t->l[i]);
        if (*p != '[')
            continue;
        p = skipws(p + 1);
        q = strchr(p, ']');
        if (!q)
            continue;
        n = q - p;
        if (n > 63)
            continue;
        memcpy(name, p, n);
        name[n] = 0;
        rtrim(name);
        if (!stricmp(name, sec))
            return i;
    }
    return -1;
}

static int ini_end(TEXT *t, int h)
{
    int e = h + 1;
    while (e < t->n && !is_section(t->l[e]))
        e++;
    return e;
}

/* line index of key in [sec] (-1 missing, -2 value too long); value into val */
static int ini_find(TEXT *t, const char *sec, const char *key, char *val)
{
    char k[64];
    int h, e, j, n;
    const char *p, *eq;

    h = ini_sec(t, sec);
    if (h < 0)
        return -1;
    e = ini_end(t, h);
    for (j = h + 1; j < e; j++) {
        p = skipws(t->l[j]);
        if (*p == ';')
            continue;
        eq = strchr(p, '=');
        if (!eq)
            continue;
        n = eq - p;
        if (n > 63)
            continue;
        memcpy(k, p, n);
        k[n] = 0;
        rtrim(k);
        if (stricmp(k, key))
            continue;
        p = skipws(eq + 1);
        if (strlen(p) >= VMAX)
            return -2;
        if (val) {
            strcpy(val, p);
            rtrim(val);
        }
        return j;
    }
    return -1;
}

static void ini_set(TEXT *t, const char *sec, const char *key, const char *val)
{
    static char line[VMAX + 80];
    int h, e, j;

    sprintf(line, "%s=%s", key, val);
    j = ini_find(t, sec, key, NULL);
    if (j >= 0) {
        t_set(t, j, line);
        return;
    }
    h = ini_sec(t, sec);
    if (h < 0) {
        if (t->n && *skipws(t->l[t->n - 1]))
            t_ins(t, t->n, "");
        sprintf(line, "[%s]", sec);
        t_ins(t, t->n, line);
        h = t->n - 1;
        sprintf(line, "%s=%s", key, val);
    }
    e = ini_end(t, h);
    while (e - 1 > h && !*skipws(t->l[e - 1]))
        e--;
    t_ins(t, e, line);
}

#ifdef UNINST
static void ini_del(TEXT *t, const char *sec, const char *key)
{
    int j = ini_find(t, sec, key, NULL);
    if (j >= 0)
        t_del(t, j);
}

static void ini_del_sec(TEXT *t, const char *sec)
{
    int h = ini_sec(t, sec), e;
    if (h < 0)
        return;
    e = ini_end(t, h);
    while (e > h)
        t_del(t, --e);
    if (h > 0 && h == t->n && !*skipws(t->l[h - 1]))
        t_del(t, h - 1);
}
#endif


/* checksum of the whole text, to leave files alone that an edit did not change */
static unsigned long t_sum(TEXT *t)
{
    unsigned long h = t->n;
    int i;
    const char *p;
    for (i = 0; i < t->n; i++) {
        for (p = t->l[i]; *p; p++) h = h * 31 + (unsigned char)*p;
        h = h * 31 + '\n';
    }
    return h;
}

static int ini_edit(const char *win, const char *file, void (*fn)(TEXT *, const char *), const char *arg)
{
    char path[PMAX];
    TEXT *t = t_new();
    unsigned long before;
    int r = 0;

    join(path, win, file);
    if (t_load(t, path)) {
        printf("  Cannot read %s\n", path);
        t_free(t);
        return -1;
    }
    before = t_sum(t);
    fn(t, arg);
    if (t_sum(t) != before)
        r = t_save(t, path);
    t_free(t);
    return r;
}

/* a WIN.INI program list (load=, run=): removes the entries whose file name (without
   extension) is 'name', then appends 'add'; 1 if changed */
static int list_edit(char *val, const char *name, const char *add)
{
    static char out[VMAX + PMAX];
    char tok[PMAX], bn[PMAX], *dot;
    const char *p = val, *s;
    int n, changed = 0;

    out[0] = 0;
    for (;;) {
        while (*p && (SP(*p) || *p == ','))
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && !SP(*p) && *p != ',')
            p++;
        n = p - s;
        if (n >= PMAX)
            n = PMAX - 1;
        memcpy(tok, s, n);
        tok[n] = 0;
        strcpy(bn, base(tok));
        if ((dot = strchr(bn, '.')) != NULL)
            *dot = 0;
        if (!stricmp(bn, name)) {
            changed = 1;
            continue;
        }
        if (out[0])
            strcat(out, " ");
        strcat(out, tok);
    }
    if (add) {
        if (out[0])
            strcat(out, " ");
        strcat(out, add);
        changed = 1;
    }
    if (strlen(out) >= VMAX)
        return 0;
    if (changed)
        strcpy(val, out);
    return changed;
}

/* ---------------------------------------------------------------- Program Manager group
   DOS cannot talk to Program Manager: the group is written to WIN.INI [WinDrv] and
   WINGRP.EXE, started once through run=, creates it over DDE. The uninstaller removes
   the group file and its PROGMAN.INI entry directly. */
#define GRPSEC  "WinDrv"
#define GRPKEY  "Group." PKG

#ifndef UNINST
#ifdef WIN_USB
static int no_sound;
#endif

static void grp_install(TEXT *t, const char *spec)
{
    char val[VMAX];
    const char *exe = spec + strlen(spec) + 1;
    int r;
    ini_set(t, GRPSEC, GRPKEY, spec);
    r = ini_find(t, "windows", "run", val);
    if (r == -2)
        return;
    if (r < 0)
        val[0] = 0;
    if (list_edit(val, "WINGRP", exe))
        ini_set(t, "windows", "run", val);
}

/* group items; 'icon' indexes the icons of WINGRP.EXE and of wicons.h */
#define IC_USB      0
#define IC_SPEAKER  1
#define IC_DISK     2
#define IC_BENCH    3
#define IC_NOTE     4
#define IC_CAMERA   5

struct gitem {
    char title[24];
    char path[PMAX];
    int icon;
};

#define MAXGITEMS 6
static struct gitem gitems[MAXGITEMS];
static int ngitems;

#ifndef WIN_UDF
static void grp_item(const char *dir, const char *file, const char *title, int icon)
{
    struct gitem *g = &gitems[ngitems];
    if (ngitems >= MAXGITEMS)
        return;
    join(g->path, dir, file);
    if (!exists(g->path))
        return;
    strcpy(g->title, title);
    g->icon = icon;
    ngitems++;
}
#endif

/* the package's text in Notepad */
static void grp_readme(const char *dir, const char *title)
{
    struct gitem *g = &gitems[ngitems];
    char f[PMAX];
    join(f, dir, PKG ".TXT");
    if (ngitems >= MAXGITEMS || !exists(f) || strlen(f) + 12 >= PMAX)
        return;
    sprintf(g->path, "NOTEPAD.EXE %s", f);
    strcpy(g->title, title);
    g->icon = IC_NOTE;
    ngitems++;
}

/* ---- Windows 3.0: Program Manager has no DDE; the group file is written here ---- */
#include "wicons.h"

static const unsigned char *const ic_and[] = { ic_usb_and, ic_speaker_and, ic_disk_and, ic_bench_and, ic_note_and,
                                               ic_camera_and };
static const unsigned char *const ic_x4[] = { ic_usb_x4, ic_speaker_x4, ic_disk_x4, ic_bench_x4, ic_note_x4,
                                              ic_camera_x4 };
static const unsigned char *const ic_x1[] = { ic_usb_x1, ic_speaker_x1, ic_disk_x1, ic_bench_x1, ic_note_x1,
                                              ic_camera_x1 };

static unsigned char grp[8192];
static unsigned glen;

static unsigned g_put(const void *p, unsigned n)
{
    unsigned at = glen;
    if (glen + n > sizeof(grp))
        return 0;
    memcpy(grp + glen, p, n);
    glen += n;
    return at;
}

static void g_w16(unsigned at, unsigned v)
{
    grp[at] = (unsigned char)v;
    grp[at + 1] = (unsigned char)(v >> 8);
}

/* icon format of the display driver in SYSTEM.INI: 4 planes (EGA, VGA, most 16-color
   drivers), 1 = monochrome, 0 = something else (no group) */
static int display_planes(const char *win, int *dpiy)
{
    char f[PMAX], drv[VMAX];
    TEXT *t = t_new();
    int planes = 4;
    *dpiy = 96;
    join(f, win, "SYSTEM.INI");
    if (t_load(t, f) == 0 && ini_find(t, "boot", "display.drv", drv) >= 0) {
        strlwr(drv);
        if (strstr(drv, "mono") || strstr(drv, "herc") || strstr(drv, "cga") || strstr(drv, "plasma") ||
            strstr(drv, "olibw") || strstr(drv, "egahibw"))
            planes = 1;
        else if (strstr(drv, "8514") || strstr(drv, "256") || strstr(drv, "tiga") || strstr(drv, "xga") ||
                 strstr(drv, "v7vga"))
            planes = 0;
        if (strstr(drv, "ega"))
            *dpiy = 72;
    }
    t_free(t);
    return planes;
}

static int grp30_write(const struct winst *w)
{
    char f[PMAX], key[16], val[VMAX];
    static unsigned char h12[12] = { 16, 0, 16, 0, 32, 0, 32, 0, 4, 0, 4, 1 };
    unsigned items, at, pname, i, k, sum;
    int planes, dpiy, n;
    FILE *fp;
    TEXT *t;

    planes = display_planes(w->dir, &dpiy);
    if (!planes) {
        printf("  no Program Manager group: icons of this display driver cannot be made from DOS\n");
        return 0;
    }
    memset(grp, 0, sizeof(grp));
    glen = 34;                                  /* header */
    memcpy(grp, "PMCC", 4);
    g_w16(8, 1);                                /* nCmdShow */
    g_w16(10, 40); g_w16(12, 40); g_w16(14, 40 + 75 * 4 + 40); g_w16(16, 40 + 140);
    g_w16(18, 0xFFFF); g_w16(20, 0xFFFF);       /* minimized position: none */
    g_w16(24, 96); g_w16(26, dpiy);
    g_w16(28, 1); g_w16(30, planes);
    g_w16(32, ngitems);
    items = glen;
    glen += 2 * ngitems;
    pname = g_put(PKG, sizeof(PKG));
    g_w16(22, pname);
    for (i = 0; i < (unsigned)ngitems; i++) {
        struct gitem *g = &gitems[i];
        at = glen;
        glen += 24;
        g_w16(items + 2 * i, at);
        g_w16(at, 21 + 75 * i);
        g_w16(at + 2, 0);
        g_w16(at + 4, 0);                       /* icon index in the program */
        g_w16(at + 6, 12);
        g_w16(at + 8, 128);
        g_w16(at + 10, planes == 4 ? 512 : 128);
        g_w16(at + 18, g_put(g->title, strlen(g->title) + 1));
        g_w16(at + 20, g_put(g->path, strlen(g->path) + 1));
        g_w16(at + 22, g_put(g->path, strlen(g->path) + 1));
        h12[10] = (unsigned char)planes;
        g_w16(at + 12, g_put(h12, 12));         /* hot spot, size, mask row bytes, format */
        g_w16(at + 14, g_put(ic_and[g->icon], 128));
        g_w16(at + 16, planes == 4 ? g_put(ic_x4[g->icon], 512) : g_put(ic_x1[g->icon], 128));
    }
    if (glen & 1)
        glen++;
    g_w16(6, glen);
    for (sum = 0, k = 0; k < glen; k += 2)
        sum += grp[k] | (grp[k + 1] << 8);
    g_w16(4, (unsigned)(0 - sum));
    sprintf(f, "%s\\%s.GRP", w->dir, PKG);
    if (!(fp = fopen(f, "wb")) || fwrite(grp, 1, glen, fp) != glen) {
        if (fp)
            fclose(fp);
        printf("  Cannot write %s\n", f);
        return -1;
    }
    fclose(fp);
    /* PROGMAN.INI [Groups]: the same file again, or the first free GroupN */
    join(val, w->dir, "PROGMAN.INI");
    t = t_new();
    if (t_load(t, val) == 0) {
        for (n = 1; n < 100; n++) {
            sprintf(key, "Group%d", n);
            if (ini_find(t, "Groups", key, val) < 0 || !stricmp(val, f))
                break;
        }
        ini_set(t, "Groups", key, f);
        join(val, w->dir, "PROGMAN.INI");
        t_save(t, val);
    }
    t_free(t);
    printf("  Program Manager group \"" PKG "\": %s\n", f);
    return 0;
}

/* the programs of the package: a Program Manager group (Windows 3.0 and 3.1, written here
   or made by WINGRP.EXE at the next start) or a Start Menu group (Windows 95/98/ME: WINGRP.EXE
   asks the shell the same way at the next start). The group is made again on every install,
   each item replaces an item of the same name: no duplicates. Windows 1.x/2.x have no
   Program Manager (MS-DOS Executive lists the files of the directory). */
static int group_install(const struct winst *w, const char *dir)
{
    static char spec[VMAX + PMAX];
    char exe[PMAX], num[8];
    int i, n;
    if (w->ver < WV_30) {
        printf("  %s has no Program Manager: the programs are in %s (MS-DOS Executive)\n", win_name(w), dir);
        return 0;
    }
    ngitems = 0;
#ifdef WIN_USB
    grp_item(dir, "USBTREE.EXE", "USB Devices", IC_USB);
    grp_item(dir, "WINCAM.EXE", "Camera", IC_CAMERA);
    if (!no_sound)
        grp_item(dir, "WINMIX.EXE", "Volume Control", IC_SPEAKER);
    grp_item(dir, "WININFO.EXE", "Disk Info", IC_DISK);
#elif defined(WIN_AUDIO)
    grp_item(dir, "WINMIX.EXE", "Volume Control", IC_SPEAKER);
    grp_item(dir, "AUDTEST.EXE", "Sound Test", IC_NOTE);
#elif defined(WIN_UDF)
#else
    grp_item(dir, "WININFO.EXE", "Disk Info", IC_DISK);
    grp_item(dir, "DSKBENCH.EXE", "Disk Benchmark", IC_BENCH);
#endif
    grp_readme(dir, PKG " Read Me");
    if (!ngitems)
        return 0;
    if (w->ver == WV_30)
        return grp30_write(w);
    /* Windows 3.1: WINGRP.EXE talks to Program Manager at the next start of Windows;
       "title|program|icon index in WINGRP.EXE;..." with the WINGRP path behind it */
    join(exe, dir, "WINGRP.EXE");
    if (!exists(exe))
        return 0;
    spec[0] = 0;
    for (i = 0; i < ngitems; i++) {
        if (i)
            strcat(spec, ";");
        sprintf(num, "%d", gitems[i].icon);
        strcat(spec, gitems[i].title);
        strcat(spec, "|");
        strcat(spec, gitems[i].path);
        strcat(spec, "|");
        strcat(spec, num);
    }
    n = strlen(spec);
    strcpy(spec + n + 1, exe);
    if (ini_edit(w->dir, "WIN.INI", grp_install, spec))
        return -1;
    printf("  %s group \"" PKG "\" (created at the next start of Windows)\n",
           IS9X(w) ? "Start Menu" : "Program Manager");
    return 0;
}
#else
static void grp_remove(TEXT *t, const char *unused)
{
    char val[VMAX];
    int h;
    (void)unused;
    ini_del(t, GRPSEC, GRPKEY);
    h = ini_sec(t, GRPSEC);
    if (h >= 0 && ini_end(t, h) == h + 1)
        ini_del_sec(t, GRPSEC);
    if (ini_sec(t, GRPSEC) < 0 && ini_find(t, "windows", "run", val) >= 0 && list_edit(val, "WINGRP", NULL))
        ini_set(t, "windows", "run", val);
}

static char grpnum[16];

static void pm_remove(TEXT *t, const char *win)
{
    char val[VMAX], key[32], order[VMAX], out[VMAX], tok[16];
    const char *p;
    int i, n;
    grpnum[0] = 0;
    for (i = 1; i < 100; i++) {
        sprintf(key, "Group%d", i);
        if (ini_find(t, "Groups", key, val) < 0 || stricmp(base(val), PKG ".GRP"))
            continue;
        if (del_file(val) == 0)
            printf("  %s deleted\n", val);
        ini_del(t, "Groups", key);
        sprintf(grpnum, "%d", i);
        break;
    }
    if (!grpnum[0] || ini_find(t, "Settings", "Order", order) < 0)
        return;
    out[0] = 0;
    for (p = order;;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        n = 0;
        while (*p && *p != ' ' && n < 15)
            tok[n++] = *p++;
        tok[n] = 0;
        if (!strcmp(tok, grpnum))
            continue;
        if (out[0])
            strcat(out, " ");
        strcat(out, tok);
    }
    ini_set(t, "Settings", "Order", out);
    (void)win;
}

/* Windows 95/98/ME: the Start Menu folder of the group, through the 8.3 names DOS sees
   ("Start Menu" is STARTM~1 and the like, the shortcuts *.LNK) */
static void startmenu_remove(const char *win)
{
    struct find_t f1, f2;
    char pat[PMAX], d[PMAX], g[PMAX], fl[PMAX];
    if (strlen(win) + 40 >= PMAX)
        return;
    sprintf(pat, "%s\\*.*", win);
    if (_dos_findfirst(pat, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM | _A_RDONLY, &f1))
        return;
    do {
        if (!(f1.attrib & _A_SUBDIR) || strnicmp(f1.name, "STARTM", 6))
            continue;
        sprintf(g, "%s\\%s\\PROGRAMS\\" PKG, win, f1.name);
        if (!isdir(g))
            continue;
        sprintf(d, "%s\\*.*", g);
        if (!_dos_findfirst(d, _A_HIDDEN | _A_SYSTEM | _A_RDONLY, &f2))
            do {
                join(fl, g, f2.name);
                _dos_setfileattr(fl, 0);
                remove(fl);
            } while (!_dos_findnext(&f2));
        if (rmdir(g) == 0)
            printf("  Start Menu group %s removed\n", g);
    } while (!_dos_findnext(&f1));
}

static int group_remove(const struct winst *w)
{
    char f[PMAX];
    if (w->ver < WV_30)
        return 0;
    if (ini_edit(w->dir, "WIN.INI", grp_remove, NULL))
        return -1;
    if (IS9X(w)) {
        startmenu_remove(w->dir);
        return 0;
    }
    join(f, w->dir, "PROGMAN.INI");
    if (exists(f) && ini_edit(w->dir, "PROGMAN.INI", pm_remove, w->dir))
        return -1;
    return 0;
}
#endif

#if defined(WIN_AUDIO) || defined(WIN_USB)
/* ---- sound: SYSTEM.INI [drivers] and the MIDI Mapper (WINAUDIO, and WINUSB for USB Audio) ----
   wave=, midi= and aux= load WINSOUND.DRV. A driver already on one of these keys moves to
   the first free numbered key (wave1= ...) and the uninstaller moves it back. The "General
   MIDI" setup of MIDIMAP.CFG is pointed at the WIN synthesizer on all 16 channels and
   selected. Both packages share this part; it is removed with the last of them. */
#define SNDDRV   "winsound.drv"
#define SNDDESC  "WINSOUND HD Audio, AC'97 and USB Audio"
#define SYNTHNAME "WIN GM Synth"
#define USBSYNTH  "WIN USB GM Synth"
#define SNDBAK    "WNS"
#define SNDSEC    "WINSOUND"
#define BEEPDRV   "winbeep.drv"
static const char *const drvkeys[] = { "wave", "midi", "aux", NULL };

#ifndef UNINST
static void drv_install(TEXT *t, const char *win30)
{
    char cur[VMAX], key[16];
    int i, k;
    if (win30 && ini_find(t, "boot", "sound.drv", cur) >= 0 && stricmp(cur, BEEPDRV)) {
        /* Windows 3.0: MessageBeep and the voice API on the synthesizer, in real mode too */
        ini_set(t, SNDSEC, "OldSound", cur);
        ini_set(t, "boot", "sound.drv", BEEPDRV);
        printf("  [boot] sound.drv=%s (was %s)\n", BEEPDRV, cur);
    }
    for (i = 0; drvkeys[i]; i++) {
        if (ini_find(t, "drivers", drvkeys[i], cur) >= 0 && cur[0] && stricmp(cur, SNDDRV)) {
            for (k = 1; k < 10; k++) {
                sprintf(key, "%s%d", drvkeys[i], k);
                if (ini_find(t, "drivers", key, NULL) < 0)
                    break;
            }
            if (k < 10) {
                ini_set(t, "drivers", key, cur);
                ini_set(t, SNDSEC, drvkeys[i], key);
                printf("  [drivers] %s=%s (was %s=)\n", key, cur, drvkeys[i]);
            } else
                warn("no free [drivers] key for %s; it is replaced", cur);
        }
        ini_set(t, "drivers", drvkeys[i], SNDDRV);
        printf("  [drivers] %s=%s\n", drvkeys[i], SNDDRV);
    }
    ini_set(t, "drivers.desc", SNDDRV, SNDDESC);
}
#else
static void drv_remove(TEXT *t, const char *unused)
{
    char cur[VMAX], key[16], old[VMAX];
    int i;
    (void)unused;
    if (ini_find(t, "boot", "sound.drv", cur) >= 0 && !stricmp(cur, BEEPDRV)) {
        if (ini_find(t, SNDSEC, "OldSound", old) < 0 || !old[0])
            strcpy(old, "sound.drv");
        ini_set(t, "boot", "sound.drv", old);
        printf("  [boot] sound.drv=%s restored\n", old);
    }
    for (i = 0; drvkeys[i]; i++) {
        if (ini_find(t, "drivers", drvkeys[i], cur) < 0 || stricmp(cur, SNDDRV))
            continue;
        if (ini_find(t, SNDSEC, drvkeys[i], key) >= 0 && key[0] &&
            ini_find(t, "drivers", key, old) >= 0 && old[0]) {
            ini_set(t, "drivers", drvkeys[i], old);
            ini_del(t, "drivers", key);
            printf("  [drivers] %s=%s restored\n", drvkeys[i], old);
        } else {
            ini_del(t, "drivers", drvkeys[i]);
            printf("  [drivers] %s= removed\n", drvkeys[i]);
        }
    }
    ini_del(t, "drivers.desc", SNDDRV);
    ini_del_sec(t, SNDSEC);
}
#endif

/* MIDIMAP.CFG (MIDI Mapper of Windows 3.1 and of the Multimedia Extensions for Windows 3.0)
   header: 1, ?, 0, current setup id, then the offsets of the three directories (setups 0Eh,
   patch maps 152Ah, key maps 2A46h). A directory is { max 100, count, 100 entries of
   { name[16], description[32], id, data offset } }; the data area starts at 3F62h.
   Setup data: 16 channels of { destination channel, port name[32], patch map id, flags (1 =
   active), 0 }, followed by 4 zero bytes. */
#if !defined(UNINST) || defined(WIN_AUDIO)
#define MM_SETUPS   0x0E
#define MM_PATCHES  0x152A
#define MM_KEYS     0x2A46
#define MM_DATA     0x3F62
#define MM_ENT      54
#define MM_SETUPLEN (16 * 40 + 4)

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return rd16(p) | ((unsigned long)rd16(p + 2) << 16); }
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static unsigned char *read_all(const char *path, unsigned *len, unsigned extra)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 60000L || !(b = calloc(1, (unsigned)n + extra))) {
        fclose(f);
        return NULL;
    }
    *len = (unsigned)fread(b, 1, (unsigned)n, f);
    fclose(f);
    return b;
}

/* a MIDIMAP.CFG with no setups, patch maps or key maps */
static unsigned char *midimap_new(unsigned *len)
{
    unsigned char *b = calloc(1, MM_DATA + MM_SETUPLEN);
    if (!b) return NULL;
    wr16(b, 1);
    wr16(b + 8, MM_SETUPS);
    wr16(b + 10, MM_PATCHES);
    wr16(b + 12, MM_KEYS);
    wr16(b + MM_SETUPS, 100);
    wr16(b + MM_PATCHES, 100);
    wr16(b + MM_KEYS, 100);
    *len = MM_DATA;
    return b;
}

/* adds a "General MIDI" setup at the end of the file; its directory entry or NULL */
static unsigned char *midimap_add(unsigned char *b, unsigned *len)
{
    unsigned char *e, *free_e = NULL;
    unsigned i, id = 0, n = rd16(b + MM_SETUPS + 2);
    if (n >= 100) return NULL;
    for (i = 0; i < 100; i++) {
        e = b + MM_SETUPS + 4 + i * MM_ENT;
        if (!e[0]) {
            if (!free_e) free_e = e;
        } else if (rd16(e + 48) > id)
            id = rd16(e + 48);
    }
    if (!free_e) return NULL;
    memset(free_e, 0, MM_ENT);
    strcpy((char *)free_e, "General MIDI");
    strcpy((char *)free_e + 16, "General MIDI setup");
    wr16(free_e + 48, id + 1);
    wr16(free_e + 50, *len);
    wr16(free_e + 52, 0);
    memset(b + *len, 0, MM_SETUPLEN);
    *len += MM_SETUPLEN;
    wr16(b + MM_SETUPS + 2, n + 1);
    return free_e;
}

static int midimap_install(const char *win, const char *synth)
{
    char path[PMAX], bak[PMAX];
    unsigned char *b, *e = NULL;
    unsigned len, i, c;
    unsigned long off;
    int created = 0;
    FILE *f;
    sprintf(path, "%s\\SYSTEM\\MIDIMAP.CFG", win);
    with_ext(bak, path, SNDBAK);
    b = read_all(path, &len, MM_SETUPLEN);
    if (b && (len < MM_DATA || rd16(b) != 1 || rd16(b + 8) != MM_SETUPS || rd16(b + 10) != MM_PATCHES ||
              rd16(b + 12) != MM_KEYS)) {
        free(b);
        warn("%s has an unknown format; choose the WIN synthesizer in the MIDI Mapper yourself", path);
        return 0;
    }
    if (!b) {
        /* Multimedia Extensions for Windows 3.0 without a sound card: no MIDIMAP.CFG yet */
        if (exists(path) || !(b = midimap_new(&len))) {
            warn("cannot read %s; choose the WIN synthesizer in the MIDI Mapper yourself", path);
            return 0;
        }
        created = 1;
    }
    for (i = 0; i < 100; i++) {
        unsigned char *s = b + MM_SETUPS + 4 + i * MM_ENT;
        if (!strcmp((char *)s, "General MIDI")) {
            e = s;
            break;
        }
    }
    if (!e && !(e = midimap_add(b, &len))) {
        free(b);
        warn("no room for a \"General MIDI\" setup in %s; choose the WIN synthesizer in the MIDI Mapper yourself",
             path);
        return 0;
    }
    off = rd32(e + 50);
    if (off < MM_DATA || off + 16 * 40UL > len) {
        free(b);
        warn("%s is damaged; choose the WIN synthesizer in the MIDI Mapper yourself", path);
        return 0;
    }
    /* the original is kept for UNINAUD; an empty backup means that there was none */
    if (!exists(bak)) {
        if (created) {
            if ((f = fopen(bak, "wb")) != NULL) fclose(f);
        } else if (copy_file(path, bak)) {
            free(b);
            return -1;
        }
    }
    for (c = 0; c < 16; c++) {
        unsigned char *ch = b + (unsigned)off + c * 40;
        memset(ch, 0, 40);
        ch[0] = (unsigned char)c;
        strcpy((char *)ch + 2, synth);
        ch[36] = 1;                 /* active, no patch map */
    }
    b[6] = e[48];                   /* current setup */
    b[7] = e[49];
    if (!(f = fopen(path, "wb")) || fwrite(b, 1, len, f) != len) {
        if (f) fclose(f);
        free(b);
        printf("  Cannot write %s\n", path);
        return -1;
    }
    fclose(f);
    free(b);
    printf("  MIDI Mapper: %s\"General MIDI\" -> %s\n", created ? "new MIDIMAP.CFG, setup " : "setup ", synth);
    return 0;
}
#endif

#if defined(UNINST) || defined(WIN_USB)
/* 1 if CONFIG.SYS loads the driver 'name' */
static int cfg_has(const char *name)
{
    char cfg[PMAX];
    TEXT *t = t_new();
    int i, r = 0;
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
    if (t_load(t, cfg) == 0)
        for (i = 0; i < t->n && !r; i++)
            if (dev_line(t->l[i], name, NULL, NULL)) r = 1;
    t_free(t);
    return r;
}
#endif

#ifndef UNINST
static int sound_install(const char *win, const char *dir, const char *synth, int win30)
{
    char src[PMAX], dst[PMAX];
    join(src, dir, "WINSOUND.DRV");
    if (!exists(src)) return 0;
    sprintf(dst, "%s\\SYSTEM\\WINSOUND.DRV", win);
    if (copy_file(src, dst)) {
        printf("  Cannot copy WINSOUND.DRV to %s\n", dst);
        return -1;
    }
    printf("  WINSOUND.DRV -> %s\n", dst);
    if (win30) {
        join(src, dir, "WINBEEP.DRV");
        sprintf(dst, "%s\\SYSTEM\\WINBEEP.DRV", win);
        if (!exists(src) || copy_file(src, dst))
            win30 = 0;
        else
            printf("  WINBEEP.DRV -> %s\n", dst);
    }
    if (ini_edit(win, "SYSTEM.INI", drv_install, win30 ? "" : NULL))
        return -1;
    return midimap_install(win, synth);
}
#else
static int sound_remove(const char *win)
{
    char f[PMAX], bak[PMAX];
#ifdef WIN_USB
    if (cfg_has("WINAUDIO.SYS")) {
        printf("  WINSOUND.DRV stays for WINAUDIO.SYS\n");
        return 0;
    }
#else
    if (cfg_has("WINUSB.SYS")) {
        printf("  WINSOUND.DRV stays for the USB audio devices of WINUSB.SYS\n");
        return midimap_install(win, USBSYNTH);
    }
#endif
    if (ini_edit(win, "SYSTEM.INI", drv_remove, NULL))
        return -1;
    sprintf(f, "%s\\SYSTEM\\MIDIMAP.CFG", win);
    with_ext(bak, f, SNDBAK);
    if (file_size(bak) == 0) {
        /* made by the installer: there was no MIDIMAP.CFG before */
        if (del_file(f) == 0)
            printf("  %s deleted\n", f);
        del_file(bak);
    } else if (exists(bak) && copy_file(bak, f) == 0) {
        del_file(bak);
        printf("  %s restored\n", f);
    }
    sprintf(f, "%s\\SYSTEM\\WINSOUND.DRV", win);
    if (del_file(f) == 0)
        printf("  %s deleted\n", f);
    sprintf(f, "%s\\SYSTEM\\WINBEEP.DRV", win);
    if (del_file(f) == 0)
        printf("  %s deleted\n", f);
    return 0;
}
#endif

#ifdef WIN_AUDIO
#ifndef UNINST
static int w9x_install(const struct winst *w, const char *dir);

static int windows_install(const struct winst *w, const char *dir)
{
    if (IS9X(w))
        return w9x_install(w, dir);
    return sound_install(w->dir, dir, SYNTHNAME, w->ver == WV_30);
}
#else
static int w9x_remove(const struct winst *w);

static int windows_remove(const struct winst *w)
{
    if (IS9X(w))
        return w9x_remove(w);
    return sound_remove(w->dir);
}
#endif
#endif
#endif /* sound */

#ifdef WIN_USB
#ifndef UNINST
static void sys_install(TEXT *t, const char *unused)
{
    char cur[VMAX];
    (void)unused;
    if (ini_find(t, "boot", "mouse.drv", cur) < 0)
        cur[0] = 0;
    if (stricmp(cur, "winmouse.drv")) {
        /* remember the previous driver for UNINUSB */
        ini_set(t, "WINUSB", "OldMouse", cur);
        if (ini_find(t, "boot.description", "mouse.drv", cur) < 0)
            cur[0] = 0;
        ini_set(t, "WINUSB", "OldMouseDesc", cur);
    }
    ini_set(t, "boot", "mouse.drv", "winmouse.drv");
    ini_set(t, "boot.description", "mouse.drv", MOUSEDESC);
    printf("  [boot] mouse.drv=winmouse.drv\n");
}

static void win_install(TEXT *t, const char *wheel)
{
    char val[VMAX];
    int r = ini_find(t, "windows", "load", val);
    if (r == -2) {
        warn("the load= line in WIN.INI is too long; add %s to it yourself", wheel);
    } else {
        if (r < 0)
            val[0] = 0;
        if (list_edit(val, "WINWHEEL", wheel)) {
            ini_set(t, "windows", "load", val);
            printf("  [windows] load=%s\n", val);
        } else
            warn("the load= line in WIN.INI is too long; add %s to it yourself", wheel);
    }
    if (ini_find(t, "WinWheel", "Lines", NULL) < 0)
        ini_set(t, "WinWheel", "Lines", "3");
    if (ini_find(t, "WinWheel", "Invert", NULL) < 0)
        ini_set(t, "WinWheel", "Invert", "0");
}

static int w9x_install(const struct winst *w, const char *dir);
static void drv9x_install(TEXT *t, const char *unused);

static int windows_install(const struct winst *w, const char *dir)
{
    char src[PMAX], dst[PMAX], wheel[PMAX];
    const char *win = w->dir;

    if (IS9X(w))
        return w9x_install(w, dir);
    if (w->ver < WV_30) {
        /* the drivers of Windows 1.x/2.x are bound into WIN100/WIN200.BIN by their Setup;
           USB keyboards and mice reach them through the PS/2 (8042) port. The 32-bit core
           runs in DOS and in the real-mode copy of Windows; Windows/386 (WIN386) puts the
           CPU in V86 mode without a VCPI server, so the core is suspended while it runs. */
        if (w->ver == WV_1X)
            printf("  USB keyboards and USB disks work; Windows 1.0x has no PS/2 mouse driver.\n");
        else
            printf("  USB keyboards, mice and disks work; the mouse needs the PS/2 mouse driver\n"
                   "  (\"Microsoft Mouse connected to PS/2 Mouse Port\" in Setup).\n");
        if (w->ver == WV_386)
            printf("  Windows/386 (WIN386) suspends USB while it runs (V86 mode, no VCPI);\n"
                   "  run the real-mode copy (WIN86.COM) for USB inside Windows.\n");
        return 0;
    }
    join(src, dir, "WINMOUSE.DRV");
    sprintf(dst, "%s\\SYSTEM\\WINMOUSE.DRV", win);
    if (copy_file(src, dst)) {
        printf("  Cannot copy WINMOUSE.DRV to %s\n", dst);
        return -1;
    }
    printf("  WINMOUSE.DRV -> %s\n", dst);
    if (ini_edit(win, "SYSTEM.INI", sys_install, NULL))
        return -1;
    join(wheel, dir, "WINWHEEL.EXE");
    if (ini_edit(win, "WIN.INI", win_install, wheel))
        return -1;
    /* USB Audio devices: WINSOUND.DRV (the WINAUDIO synthesizer is preferred when installed) */
    if (no_sound)
        return 0;
    return sound_install(win, dir, cfg_has("WINAUDIO.SYS") ? SYNTHNAME : USBSYNTH, w->ver == WV_30);
}
#else
static int mouse_restored;

static void sys_remove(TEXT *t, const char *win)
{
    char cur[VMAX], old[VMAX], desc[VMAX], f[PMAX];

    mouse_restored = 0;
    if (ini_find(t, "boot", "mouse.drv", cur) >= 0 && !stricmp(cur, "winmouse.drv")) {
        if (ini_find(t, "WINUSB", "OldMouse", old) < 0)
            strcpy(old, DEFMOUSE);
        if (ini_find(t, "WINUSB", "OldMouseDesc", desc) < 0)
            strcpy(desc, DEFDESC);
        if (old[0]) {
            ini_set(t, "boot", "mouse.drv", old);
            printf("  [boot] mouse.drv=%s\n", old);
            sprintf(f, "%s\\SYSTEM\\%s", win, old);
            if (!exists(f))
                warn("%s is missing; choose a mouse in Windows Setup", f);
        } else
            ini_del(t, "boot", "mouse.drv");
        if (desc[0])
            ini_set(t, "boot.description", "mouse.drv", desc);
        else
            ini_del(t, "boot.description", "mouse.drv");
    }
    if (ini_find(t, "boot", "mouse.drv", cur) < 0 || stricmp(cur, "winmouse.drv"))
        mouse_restored = 1;
    ini_del_sec(t, "WINUSB");
}

static void win_remove(TEXT *t, const char *unused)
{
    char val[VMAX];
    (void)unused;
    if (ini_find(t, "windows", "load", val) >= 0 && list_edit(val, "WINWHEEL", NULL)) {
        ini_set(t, "windows", "load", val);
        printf("  [windows] load=%s\n", val);
    }
    ini_del_sec(t, "WinWheel");
}

static int w9x_remove(const struct winst *w);

static int windows_remove(const struct winst *w)
{
    char f[PMAX];
    char *win = (char *)w->dir;

    if (w->ver < WV_30)
        return 0;
    if (IS9X(w))
        return w9x_remove(w);
    if (ini_edit(win, "SYSTEM.INI", sys_remove, win))
        return -1;
    if (ini_edit(win, "WIN.INI", win_remove, NULL))
        return -1;
    sprintf(f, "%s\\SYSTEM\\WINMOUSE.DRV", win);
    if (mouse_restored && del_file(f) == 0)
        printf("  %s deleted\n", f);
    return sound_remove(win);
}
#endif
#endif /* WIN_USB */

/* ---------------------------------------------------------------- Windows 95/98/ME */
#if defined(WIN_USB) || defined(WIN_AUDIO) || defined(WIN_DISKPKG) || defined(WIN_UDF)
/* 1 if the line is device=<name> (name compared without path) */
static int device_is(const char *ln, const char *name)
{
    char v[PMAX];
    const char *p = skipws(ln), *eq;
    if (strnicmp(p, "device", 6))
        return 0;
    p = skipws(p + 6);
    if (*p != '=')
        return 0;
    eq = skipws(p + 1);
    if (strlen(eq) >= PMAX)
        return 0;
    strcpy(v, eq);
    rtrim(v);
    return !stricmp(base(v), base(name)) || !stricmp(v, name);
}


/* [386Enh] device=<name>: WIN4G.VXD moves the PCI resources above 4G below 4G at every start,
   before the Plug and Play PCI enumerator runs (Windows ME has no CONFIG.SYS driver that
   could do it); WINDISK.VXD loaded at the start takes the disk controllers of the BIOS drives
   before the I/O Supervisor looks at them */
#define W4G "win4g.vxd"
#define WDV "windisk.vxd"
#define WUV "winusb.vxd"
#define WFV "winudf.vxd"
#ifndef UNINST
static void dev_install(TEXT *t, const char *name)
{
    int h = ini_sec(t, "386Enh"), e, j;
    char line[PMAX + 8];
    if (h < 0) {
        ini_set(t, "386Enh", "device", name);
    } else {
        e = ini_end(t, h);
        for (j = h + 1; j < e; j++)
            if (device_is(t->l[j], name))
                return;
        while (e - 1 > h && !*skipws(t->l[e - 1]))
            e--;
        sprintf(line, "device=%s", name);
        t_ins(t, e, line);
    }
    printf("  [386Enh] device=%s\n", name);
}
#endif
#if defined(UNINST) || defined(WIN_DISKPKG)
static void dev_remove(TEXT *t, const char *name)
{
    int h = ini_sec(t, "386Enh"), e, j;
    if (h < 0)
        return;
    e = ini_end(t, h);
    for (j = h + 1; j < e; j++)
        if (device_is(t->l[j], name)) {
            t_del(t, j);
            printf("  [386Enh] device=%s removed\n", name);
            return;
        }
}
#endif
#ifndef UNINST
#if defined(WIN_USB) || defined(WIN_AUDIO)
/* [drivers] wave= and midi= of SYSTEM.INI: WINSOUND.DRV finds the VxD by itself */
static void drv9x_install(TEXT *t, const char *unused)
{
    char cur[VMAX];
    (void)unused;
    if (ini_find(t, "drivers", "wave", cur) >= 0 && cur[0] && stricmp(cur, SNDDRV))
        ini_set(t, SNDSEC, "wave", cur);
    if (ini_find(t, "drivers", "midi", cur) >= 0 && cur[0] && stricmp(cur, SNDDRV))
        ini_set(t, SNDSEC, "midi", cur);
    ini_set(t, "drivers", "wave", SNDDRV);
    ini_set(t, "drivers", "midi", SNDDRV);
    ini_set(t, "drivers.desc", SNDDRV, SNDDESC);
    printf("  [drivers] wave=%s midi=%s\n", SNDDRV, SNDDRV);
}
#endif

/* a file Windows has open (the loaded VxD, the mouse driver): the copy goes next to it and
   WININIT.INI renames it at the next start of Windows */
static int w9x_copy_later(const char *src, const char *dst, const char *win)
{
    char tmp[PMAX], ini[PMAX];
    const char *e = strrchr(base(dst), '.');
    FILE *fp;
    strcpy(tmp, dst);
    sprintf(tmp + (e ? e - dst : (int)strlen(dst)), ".~%.2s", e ? e + 1 : "");
    if (copy_file(src, tmp))
        return -1;
    sprintf(ini, "%s\\WININIT.INI", win);
    fp = fopen(ini, "r+");
    if (!fp) {
        fp = fopen(ini, "w");
        if (fp)
            fputs("[rename]\r\n", fp);
    } else {
        fseek(fp, 0, SEEK_END);
    }
    if (!fp)
        return -1;
    fprintf(fp, "%s=%s\r\n", dst, tmp);
    fclose(fp);
    return 0;
}

/* the INF files in the INF directory of Windows: the files are in place already, so the
   CopyFiles lines go away. Windows takes the source of an INF found there from its own
   setup directory and would ask for the WINDRV disk for every file. The INF files of the
   package directory keep them for Have Disk. */
static int inf_copy(const char *src, const char *dst)
{
    TEXT *t = t_new();
    FILE *f;
    const char *p;
    int i, bad = 0;
    if (t_load(t, src)) {
        t_free(t);
        return -1;
    }
    f = fopen(dst, "wb");
    if (!f) {
        t_free(t);
        return -1;
    }
    fputs("; installed by " INSTEXE ": the files are in place, the CopyFiles lines are left out\r\n", f);
    for (i = 0; i < t->n; i++) {
        p = skipws(t->l[i]);
        if (!strnicmp(p, "CopyFiles", 9) && (SP(p[9]) || p[9] == '='))
            continue;
        if (fputs(t->l[i], f) == EOF || fputs("\r\n", f) == EOF)
            bad = 1;
    }
    if (fclose(f))
        bad = 1;
    t_free(t);
    return bad ? -1 : 0;
}

static int w9x_install(const struct winst *w, const char *dir)
{
    char src[PMAX], dst[PMAX];
    int i, n = 0, later = 0;
    for (i = 0; w9xfiles[i].name; i++) {
        join(src, dir, w9xfiles[i].name);
        if (!exists(src))
            continue;
        w9x_path(dst, w->dir, w9xfiles[i].where, w9xfiles[i].name);
        if (w9xfiles[i].where == 17) {
            if (inf_copy(src, dst)) {
                printf("  Cannot write %s\n", dst);
                return -1;
            }
        } else if (copy_file(src, dst)) {
            if (w9x_copy_later(src, dst, w->dir)) {
                printf("  Cannot copy %s to %s\n", src, dst);
                return -1;
            }
            later++;
        }
        n++;
    }
    printf("  %d files -> %s\\SYSTEM, SYSTEM\\IOSUBSYS and INF\n", n, w->dir);
#ifdef WIN_AUDIO
    {
        int k;
        sprintf(dst, "%s\\SYSTEM", w->dir);
        k = fw_copy(dir, dst);
        if (k < 0)
            return -1;
        if (k)
            printf("  %d firmware files -> %s\\" FWDIR "\n", k, dst);
    }
#endif
    if (later)
        printf("  %d of them are in use and get replaced at the next start of Windows (WININIT.INI)\n", later);
#ifdef WIN_USB
    if (ini_edit(w->dir, "SYSTEM.INI", sys_install, NULL))
        return -1;
    join(src, dir, "WINWHEEL.EXE");
    if (ini_edit(w->dir, "WIN.INI", win_install, src))
        return -1;
    if (!no_sound && ini_edit(w->dir, "SYSTEM.INI", drv9x_install, NULL))
        return -1;
#elif defined(WIN_AUDIO)
    if (ini_edit(w->dir, "SYSTEM.INI", drv9x_install, NULL))
        return -1;
#endif
#ifdef WIN_UDF
    /* the file system driver is a static VxD; Windows 98/ME's own UDF.VXD (UDF 1.02, read
       only) stays: WINUDF.VXD is asked first */
    if (ini_edit(w->dir, "SYSTEM.INI", dev_install, WFV))
        return -1;
    printf("  UDF volumes of CD/DVD/BD drives and disks after the next start of Windows.\n");
#else
    if (ini_edit(w->dir, "SYSTEM.INI", dev_install, W4G))
        return -1;
#ifdef WIN_DISKPKG
    /* the disks of Windows itself with 32-bit access from the start; with /DOS, WINDISK.SYS
       of CONFIG.SYS loads the VxD and its DOS drives stay in MS-DOS compatibility mode */
    if (ini_edit(w->dir, "SYSTEM.INI", dosdrv ? dev_remove : dev_install, WDV))
        return -1;
#elif defined(WIN_USB)
    /* the USB host controllers before the I/O Supervisor: a USB boot disk with 32-bit access */
    if (ini_edit(w->dir, "SYSTEM.INI", dev_install, WUV))
        return -1;
#endif
    printf("  Plug and Play: Windows loads the driver from the INF files.\n"
           "  Open Device Manager (Control Panel, System), select the controller\n"
#ifdef WIN_USB
           "  (\"PCI Universal Serial Bus\", \"PCI Card\" or the Microsoft USB controller),\n"
#elif defined(WIN_DISKPKG)
           "  (\"Standard Dual PCI IDE Controller\", \"PCI Mass Storage Controller\",\n"
           "  \"PCI SD Host Controller\" or \"Other devices\"),\n"
#else
           "  (\"PCI Multimedia Audio Device\" or the sound card),\n"
#endif
           "  Properties, Driver, Change/Update Driver, Have Disk, and give\n"
           "  %s\\INF. A controller Windows has not seen yet gets the\n"
           "  WINDRV driver offered at the next start.\n", w->dir);
#endif
    return 0;
}
#else
#if defined(WIN_USB) || defined(WIN_AUDIO)
static void drv9x_remove(TEXT *t, const char *unused)
{
    char cur[VMAX], old[VMAX];
    (void)unused;
    if (ini_find(t, "drivers", "wave", cur) >= 0 && !stricmp(cur, SNDDRV)) {
        if (ini_find(t, SNDSEC, "wave", old) >= 0 && old[0])
            ini_set(t, "drivers", "wave", old);
        else
            ini_del(t, "drivers", "wave");
    }
    if (ini_find(t, "drivers", "midi", cur) >= 0 && !stricmp(cur, SNDDRV)) {
        if (ini_find(t, SNDSEC, "midi", old) >= 0 && old[0])
            ini_set(t, "drivers", "midi", old);
        else
            ini_del(t, "drivers", "midi");
    }
    ini_del(t, "drivers.desc", SNDDRV);
    ini_del_sec(t, SNDSEC);
    printf("  [drivers] wave= and midi= restored\n");
}
#endif

static int w9x_remove(const struct winst *w)
{
    char f[PMAX];
    int i, n = 0;
#ifdef WIN_UDF
    if (ini_edit(w->dir, "SYSTEM.INI", dev_remove, WFV))
        return -1;
#else
    printf("  Remove the WINDRV devices in Device Manager first; the files go now.\n");
#endif
#ifdef WIN_USB
    if (ini_edit(w->dir, "SYSTEM.INI", sys_remove, (char *)w->dir))
        return -1;
    if (ini_edit(w->dir, "WIN.INI", win_remove, NULL))
        return -1;
#endif
#if defined(WIN_USB) || defined(WIN_AUDIO)
    if (ini_edit(w->dir, "SYSTEM.INI", drv9x_remove, NULL))
        return -1;
#endif
#ifndef WIN_UDF
    /* WIN4G.VXD stays while another WINDRV package's VxD is installed */
    if (!w9x_other(w->dir, P_DISK | P_USB | P_AUD) && ini_edit(w->dir, "SYSTEM.INI", dev_remove, W4G))
        return -1;
#endif
#ifdef WIN_DISKPKG
    if (ini_edit(w->dir, "SYSTEM.INI", dev_remove, WDV))
        return -1;
#elif defined(WIN_USB)
    if (ini_edit(w->dir, "SYSTEM.INI", dev_remove, WUV))
        return -1;
#endif
    for (i = 0; w9xfiles[i].name; i++) {
        w9x_path(f, w->dir, w9xfiles[i].where, w9xfiles[i].name);
        /* files shared with another package while its VxD is installed */
        if (!stricmp(w9xfiles[i].name, "WIN4G.VXD") ? w9x_other(w->dir, P_DISK | P_USB | P_AUD) :
            (!stricmp(w9xfiles[i].name, "WINSOUND.DRV") || !stricmp(w9xfiles[i].name, "WINMIX.EXE")) ?
            w9x_other(w->dir, P_USB | P_AUD) : 0)
            continue;
        if (del_file(f) == 0)
            n++;
    }
#ifdef WIN_AUDIO
    sprintf(f, "%s\\SYSTEM", w->dir);
    n += fw_remove(f);
#endif
    printf("  %d files deleted\n", n);
    return 0;
}
#endif
#endif

#if defined(WIN_DISKPKG) || defined(WIN_USB) || defined(WIN_UDF)
#if !defined(WIN_UDF)
#define WIN_CDPKG
#endif
/* ---- CD/DVD drives: MSCDEX in AUTOEXEC.BAT, LASTDRIVE=Z in CONFIG.SYS ----
   WINCD.SYS (after WINDISK.SYS) is the CD-ROM device WINCD001 for MSCDEX, WINUCD.SYS
   (after WINUSB.SYS) the device WINUCD01 of the USB CD/DVD drives. An MSCDEX line gets
   the /D: option of the device; without one a new line is written when MSCDEX.EXE is
   found. CONFIG.SYS gets LASTDRIVE=Z when it has no LASTDRIVE line, so that MSCDEX finds
   free drive letters (a marker file in the driver directory tells the uninstaller to take
   it away again). */
#ifdef WIN_UDF
#define LDMARK  "LASTDRV.WNF"
#elif defined(WIN_USB)
#define CDOPT   "/D:WINUCD01"
#define CDSYS   "WINUCD.SYS"
#define CDDEV   "WINUCD01"
#define LDMARK  "LASTDRV.WNU"
#else
#define CDOPT   "/D:WINCD001"
#define CDSYS   "WINCD.SYS"
#define CDDEV   "WINCD001"
#define LDMARK  "LASTDRV.WND"
#endif

/* the first word of a batch line without LH/LOADHIGH and their options */
static const char *cmd_word(const char *ln, char *tok)
{
    const char *p = skipws(ln);
    int n;
    for (;;) {
        n = 0;
        while (p[n] && !SP(p[n]) && n < PMAX - 1) {
            tok[n] = p[n];
            n++;
        }
        tok[n] = 0;
        if (stricmp(tok, "LH") && stricmp(tok, "LOADHIGH") && tok[0] != '/')
            return p + n;
        p = skipws(p + n);
    }
}

static int is_cmd(const char *ln, const char *name)
{
    char tok[PMAX], b[PMAX], *dot;
    cmd_word(ln, tok);
    strcpy(b, base(tok));
    if ((dot = strchr(b, '.')) != NULL)
        *dot = 0;
    return !stricmp(b, name);
}

#ifdef WIN_CDPKG
static char *find_opt(const char *ln, const char *opt)
{
    const char *p;
    size_t n = strlen(opt);
    for (p = ln; *p; p++)
        if (!strnicmp(p, opt, n) && (!p[n] || SP(p[n])) && (p == ln || SP(p[-1])))
            return (char *)p;
    return NULL;
}

#ifdef UNINST
static int find_opt_prefix(const char *ln, const char *pre)
{
    const char *p = ln;
    size_t n = strlen(pre);
    for (; *p; p++)
        if (!strnicmp(p, pre, n) && (p == ln || SP(p[-1])))
            return 1;
    return 0;
}
#endif

#ifndef UNINST
static int find_mscdex(char *out)
{
    char path[512], *p, *e;
    sprintf(out, "%c:\\DOS\\MSCDEX.EXE", bootdrv);
    if (exists(out)) return 1;
    p = getenv("PATH");
    if (!p) return 0;
    strncpy(path, p, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    for (p = path; *p; p = e) {
        e = strchr(p, ';');
        if (e) *e++ = 0;
        else e = p + strlen(p);
        if (!*p) continue;
        join(out, p, "MSCDEX.EXE");
        if (exists(out)) return 1;
    }
    return 0;
}

static int cd_install(const char *dir)
{
    TEXT *t = t_new();
    char aut[PMAX], cfg[PMAX], line[LMAX], mx[PMAX], f[PMAX];
    int i, at = -1, r;
    FILE *fp;

    sprintf(aut, "%c:\\AUTOEXEC.BAT", bootdrv);
    printf("Updating %s (CD/DVD drives)\n", aut);
    if (t_load(t, aut) < 0) {
        printf("  Cannot read %s\n", aut);
        t_free(t);
        return -1;
    }
    for (i = 0; i < t->n; i++)
        if (is_cmd(t->l[i], "MSCDEX")) break;
    if (i < t->n) {
        if (find_opt(t->l[i], CDOPT)) {
            printf("  MSCDEX already uses " CDDEV "\n");
        } else if (strlen(t->l[i]) + 12 < LMAX) {
            sprintf(line, "%s %s", t->l[i], CDOPT);
            t_set(t, i, line);
            printf("  %s\n", line);
        }
    } else if (find_mscdex(mx)) {
        /* before SMARTDRV (it caches CDs loaded before it) and before Windows starts */
        for (i = 0; i < t->n && at < 0; i++)
            if (is_cmd(t->l[i], "SMARTDRV") || is_cmd(t->l[i], "WIN")) at = i;
        sprintf(line, "%s %s", mx, CDOPT);
        t_ins(t, at < 0 ? t->n : at, line);
        printf("  %s\n", line);
    } else {
        warn("MSCDEX.EXE was not found; for the CD/DVD drives load it with %s", CDOPT);
    }
    r = t_save(t, aut);
    t_free(t);
    if (r) return r;

    t = t_new();
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
    if (t_load(t, cfg)) {
        t_free(t);
        return 0;
    }
    /* the CD-ROM device right after every line of the driver */
    join(f, dir, CDSYS);
    sprintf(line, "DEVICE=%s", f);
    for (i = 0; i < t->n;)
        if (dev_line(t->l[i], CDSYS, NULL, NULL)) t_del(t, i);
        else i++;
    for (i = 0; i < t->n; i++)
        if (dev_line(t->l[i], SYSFILE, NULL, NULL)) {
            t_ins(t, i + 1, line);
            i++;
        }
    printf("  %s\n", line);
    for (i = 0; i < t->n; i++)
        if (!strnicmp(skipws(t->l[i]), "LASTDRIVE", 9)) break;
    if (i == t->n) {
        t_ins(t, t->n, "LASTDRIVE=Z");
        printf("  LASTDRIVE=Z\n");
        join(f, dir, LDMARK);
        if ((fp = fopen(f, "wb")) != NULL) fclose(fp);
    }
    r = t_save(t, cfg);
    t_free(t);
    return r;
}
#else
static int cd_remove(const char *dir)
{
    TEXT *t = t_new();
    char aut[PMAX], cfg[PMAX], f[PMAX], *o;
    int i, r = 0, changed = 0;

    sprintf(aut, "%c:\\AUTOEXEC.BAT", bootdrv);
    if (t_load(t, aut) == 0) {
        for (i = 0; i < t->n;) {
            if (is_cmd(t->l[i], "MSCDEX") && (o = find_opt(t->l[i], CDOPT)) != NULL) {
                memmove(o, o + strlen(CDOPT), strlen(o + strlen(CDOPT)) + 1);
                rtrim(t->l[i]);
                changed = 1;
                if (!find_opt_prefix(t->l[i], "/D:")) {
                    printf("  %s: MSCDEX line removed\n", aut);
                    t_del(t, i);
                    continue;
                }
                printf("  %s: %s\n", aut, t->l[i]);
            }
            i++;
        }
        if (changed) r = t_save(t, aut);
    }
    t_free(t);
    join(f, dir, LDMARK);
    t = t_new();
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
    changed = 0;
    if (t_load(t, cfg) == 0) {
        for (i = 0; i < t->n;)
            if (dev_line(t->l[i], CDSYS, NULL, NULL)) {
                printf("  removing: %s\n", t->l[i]);
                t_del(t, i);
                changed = 1;
            } else
                i++;
        if (dir[0] && exists(f))
            for (i = 0; i < t->n; i++)
                if (!stricmp(skipws(t->l[i]), "LASTDRIVE=Z")) {
                    t_del(t, i);
                    printf("  removing: LASTDRIVE=Z\n");
                    changed = 1;
                    break;
                }
        if (changed) r |= t_save(t, cfg);
    }
    t_free(t);
    if (dir[0]) del_file(f);
    return r;
}
#endif
#endif /* WIN_CDPKG */
#endif

#ifdef WIN_DISKPKG
/* ---- [386Enh]: 32-bit disk access through WINDISK.386 (BlockDev FastDisk) ----
   device=*int13 sends V86 INT 13h calls for drives 80h/81h to BlockDev and
   32BitDiskAccess=on tells WINDISK.386 to register them. WDCTRL cannot drive AHCI, NVMe
   or disks above 504 MB and stops Windows with an error when 32BitDiskAccess is on, so
   its line is switched off (and restored by UNINDISK). */
#define WD_MARK ";WINDISK:"

#ifndef UNINST
static void sys386_install(TEXT *t, const char *unused)
{
    char cur[VMAX];
    int h, e, j, int13 = 0;
    (void)unused;
    h = ini_sec(t, "386Enh");
    if (h < 0) {
        ini_set(t, "386Enh", "32BitDiskAccess", "on");
        h = ini_sec(t, "386Enh");
    }
    e = ini_end(t, h);
    for (j = h + 1; j < e; j++) {
        if (device_is(t->l[j], "*int13"))
            int13 = 1;
        if (device_is(t->l[j], "*wdctrl")) {
            char line[LMAX + 16];
            sprintf(line, WD_MARK "%s", t->l[j]);
            t_set(t, j, line);
            ini_set(t, "WINDISK", "Wdctrl", "1");
            printf("  [386Enh] device=*wdctrl switched off\n");
            e = ini_end(t, h);
        }
    }
    if (!int13) {
        while (e - 1 > h && !*skipws(t->l[e - 1]))
            e--;
        t_ins(t, e, "device=*int13");
        ini_set(t, "WINDISK", "Int13", "1");
        printf("  [386Enh] device=*int13\n");
    }
    if (ini_find(t, "WINDISK", "Old32Bit", NULL) < 0) {
        if (ini_find(t, "386Enh", "32BitDiskAccess", cur) < 0)
            cur[0] = 0;
        ini_set(t, "WINDISK", "Old32Bit", cur);
    }
    ini_set(t, "386Enh", "32BitDiskAccess", "on");
    printf("  [386Enh] 32BitDiskAccess=on\n");
}

static int windows_install(const struct winst *w, const char *dir)
{
    if (IS9X(w))
        return w9x_install(w, dir);
    if (w->ver < WV_30) {
        /* Windows 1.x/2.x real mode: the disks come from the DOS driver, nothing to set up.
           Windows/386 (WIN386) suspends the 32-bit core in V86 mode (no VCPI), so its disks
           are reachable only in DOS or in the real-mode copy of Windows. */
        printf("  disks work through the real-mode DOS driver; no changes needed here.\n");
        if (w->ver == WV_386)
            printf("  Windows/386 (WIN386) suspends the driver (V86 mode, no VCPI);\n"
                   "  run the real-mode copy (WIN86.COM) to use its disks inside Windows.\n");
        return 0;
    }
    if (w->ver == WV_30) {
        /* WIN386 of Windows 3.0 has no BlockDev and no *int13 */
        printf("  no 32-bit disk access in Windows 3.0, SYSTEM.INI unchanged\n");
        return 0;
    }
    return ini_edit(w->dir, "SYSTEM.INI", sys386_install, NULL);
}
#else
static void sys386_remove(TEXT *t, const char *unused)
{
    char v[VMAX];
    int h, e, j;
    (void)unused;
    h = ini_sec(t, "386Enh");
    if (h >= 0) {
        e = ini_end(t, h);
        for (j = h + 1; j < e; j++) {
            if (!strnicmp(t->l[j], WD_MARK, strlen(WD_MARK))) {
                t_set(t, j, t->l[j] + strlen(WD_MARK));
                printf("  [386Enh] device=*wdctrl restored\n");
            } else if (ini_find(t, "WINDISK", "Int13", NULL) >= 0 && device_is(t->l[j], "*int13")) {
                t_del(t, j);
                ini_del(t, "WINDISK", "Int13");
                printf("  [386Enh] device=*int13 removed\n");
                j--;
                e--;
            }
        }
    }
    if (ini_find(t, "WINDISK", "Old32Bit", v) >= 0) {
        if (v[0])
            ini_set(t, "386Enh", "32BitDiskAccess", v);
        else
            ini_del(t, "386Enh", "32BitDiskAccess");
        printf("  [386Enh] 32BitDiskAccess %s\n", v[0] ? "restored" : "removed");
    }
    ini_del_sec(t, "WINDISK");
}

static int windows_remove(const struct winst *w)
{
    if (w->ver < WV_30)
        return 0;
    if (IS9X(w))
        return w9x_remove(w);
    return ini_edit(w->dir, "SYSTEM.INI", sys386_remove, NULL);
}
#endif
#endif

#ifdef WIN_UDF
/* ---- WINUDF.EXE: in AUTOEXEC.BAT after MSCDEX (it reads the CD/DVD drives of MSCDEX) and
   before Windows starts, LASTDRIVE=Z in CONFIG.SYS for the drive letters of the UDF
   volumes. MS-DOS 7 (Windows 95/98): the DOSSTART.BAT of the MS-DOS mode instead, Windows
   itself has WINUDF.VXD. */

#ifndef UNINST
/* the place for the WINUDF line: after the last MSCDEX line, else before the line that
   starts Windows, else the end */
static int udf_at(TEXT *t)
{
    int i, at = -1;
    for (i = 0; i < t->n; i++)
        if (is_cmd(t->l[i], "MSCDEX")) at = i + 1;
    if (at >= 0) return at;
    for (i = 0; i < t->n; i++)
        if (is_cmd(t->l[i], "WIN")) return i;
    return t->n;
}

static int bat_install(const char *bat, const char *dir, const char *opts, int create)
{
    TEXT *t = t_new();
    char line[PMAX + 300];
    int i, r = t_load(t, bat);
    if (r < 0 || (r > 0 && !create)) {
        t_free(t);
        if (r < 0) printf("  Cannot read %s\n", bat);
        return r < 0 ? -1 : 0;
    }
    for (i = 0; i < t->n;)
        if (is_cmd(t->l[i], "WINUDF")) t_del(t, i);
        else i++;
    sprintf(line, "%s\\" SYSFILE "%s%s", dir, opts && *opts ? " " : "", opts ? opts : "");
    t_ins(t, udf_at(t), line);
    printf("  %s: %s\n", bat, line);
    r = t_save(t, bat);
    t_free(t);
    return r;
}

static int udf_dos_install(const char *dir, const char *opts)
{
    TEXT *t;
    char f[PMAX], cfg[PMAX];
    FILE *fp;
    int i, r;
    if (dos7() || win9x_box) {
        printf("MS-DOS 7: " SYSFILE " goes to DOSSTART.BAT (MS-DOS mode), Windows gets WINUDF.VXD\n");
        return 0;
    }
    sprintf(f, "%c:\\AUTOEXEC.BAT", bootdrv);
    printf("Updating %s\n", f);
    if (bat_install(f, dir, opts, 1))
        return -1;
    t = t_new();
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
    if (t_load(t, cfg) == 0) {
        for (i = 0; i < t->n; i++)
            if (!strnicmp(skipws(t->l[i]), "LASTDRIVE", 9)) break;
        if (i == t->n) {
            t_ins(t, t->n, "LASTDRIVE=Z");
            printf("  %s: LASTDRIVE=Z\n", cfg);
            join(f, dir, LDMARK);
            if ((fp = fopen(f, "wb")) != NULL) fclose(fp);
            r = t_save(t, cfg);
            t_free(t);
            return r;
        }
    }
    t_free(t);
    return 0;
}

static int windows_install(const struct winst *w, const char *dir)
{
    char f[PMAX];
    if (IS9X(w)) {
        if (w9x_install(w, dir))
            return -1;
        if (w->ver < WV_ME) {
            /* MS-DOS mode: Windows runs DOSSTART.BAT when it leaves for DOS */
            join(f, w->dir, "DOSSTART.BAT");
            return bat_install(f, dir, NULL, 1);
        }
        return 0;
    }
    printf("  the UDF drives of WINUDF.EXE (AUTOEXEC.BAT) are there in %s too\n", win_name(w));
    return 0;
}
#else
/* removes the WINUDF lines of a batch file; its directory into dir */
static int bat_remove(const char *bat, char *dir)
{
    TEXT *t = t_new();
    char tok[PMAX], full[PMAX];
    int i, n = 0, r = 0;
    if (t_load(t, bat)) {
        t_free(t);
        return 0;
    }
    for (i = 0; i < t->n;) {
        if (is_cmd(t->l[i], "WINUDF")) {
            printf("  %s: removing: %s\n", bat, t->l[i]);
            if (dir && !dir[0]) {
                cmd_word(t->l[i], tok);
                if (truename(tok, full)) strcpy(full, tok);
                dir_of(dir, full);
            }
            t_del(t, i);
            n++;
        } else i++;
    }
    if (n) r = t_save(t, bat);
    t_free(t);
    return r;
}

static int udf_dos_remove(char *dir)
{
    TEXT *t;
    char f[PMAX], cfg[PMAX];
    int i, r = 0;
    sprintf(f, "%c:\\AUTOEXEC.BAT", bootdrv);
    if (bat_remove(f, dir))
        return -1;
    if (!dir[0])
        return 0;
    join(f, dir, LDMARK);
    if (!exists(f))
        return 0;
    t = t_new();
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
    if (t_load(t, cfg) == 0)
        for (i = 0; i < t->n; i++)
            if (!stricmp(skipws(t->l[i]), "LASTDRIVE=Z")) {
                t_del(t, i);
                printf("  %s: removing: LASTDRIVE=Z\n", cfg);
                r = t_save(t, cfg);
                break;
            }
    t_free(t);
    del_file(f);
    return r;
}

static int windows_remove(const struct winst *w)
{
    char f[PMAX];
    if (!IS9X(w))
        return 0;
    join(f, w->dir, "DOSSTART.BAT");
    if (bat_remove(f, NULL))
        return -1;
    return w9x_remove(w);
}
#endif
#endif /* WIN_UDF */

/* ---------------------------------------------------------------- main */

static void own_dir(char *out, const char *argv0)
{
    char full[PMAX];
    if (truename(argv0, full))
        strcpy(full, argv0);
    dir_of(out, full);
}

#ifndef UNINST
static void usage(void)
{
    printf("Usage: " INSTEXE " [directory] [/W:windir]"
#ifdef WIN_USB
           " [/NOWIN] [/NOSOUND] [/DOS]"
#elif defined(WIN_AUDIO)
           " [/NOWIN] [/DOS]"
#elif defined(WIN_UDF)
           " [/NOWIN]"
#else
           " [/NO32] [/DOS]"
#endif
           " [/O options...]\n\n"
           "  directory  where to install (default: the directory of the newest Windows)\n"
           "  /W:windir  only this Windows (default: every supported Windows found)\n"
#ifdef WIN_USB
           "  /NOWIN     do not set up WINMOUSE.DRV, WINWHEEL.EXE and WINSOUND.DRV\n"
           "  /NOSOUND   do not set up WINSOUND.DRV (USB Audio devices)\n"
#elif defined(WIN_AUDIO)
           "  /NOWIN     do not set up WINSOUND.DRV and the MIDI Mapper\n"
#elif defined(WIN_UDF)
           "  /NOWIN     do not set up WINUDF.VXD and the groups of Windows\n"
#endif
#if defined(WIN_UDF)
#elif defined(WIN_USB) || defined(WIN_AUDIO)
           "  /DOS       Windows 95/98: also load " SYSFILE " from CONFIG.SYS (DOS sessions)\n"
#else
           "  /DOS       Windows 95/98: also load " SYSFILE " from CONFIG.SYS (MS-DOS mode);\n"
           "             its DOS drives then stay in MS-DOS compatibility mode in Windows\n"
#endif
#ifdef WIN_DISKPKG
           "  /NO32      leave SYSTEM.INI alone (no 32-bit disk access)\n"
#endif
           "  /O ...     everything after /O becomes the " SYSFILE " options,\n"
           "             e.g. " INSTEXE " /O "
#ifdef WIN_USB
           "/R:3"
#elif defined(WIN_AUDIO)
           "/WIN"
#elif defined(WIN_UDF)
           "/L:R"
#else
           "/PIO /ISA"
#endif
           "\n"
           "             (without /O the options of an existing installation are kept)\n");
}

int main(int argc, char **argv)
{
    char src[PMAX], dir[PMAX] = "", win[PMAX] = "", opts[256] = "";
    char from[PMAX], to[PMAX], cfg[PMAX];
    int i, useropts = 0, nowin = 0, same, copied = 0, inwin, pick;

    bootdrv = boot_drive();
    _harderr(critical_error);
    printf(PKG " setup\n\n");
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') {
            if (!stricmp(a + 1, "O")) {
                for (i++; i < argc; i++) {
                    if (strlen(opts) + strlen(argv[i]) + 2 > sizeof(opts))
                        break;
                    if (opts[0])
                        strcat(opts, " ");
                    strcat(opts, argv[i]);
                }
                useropts = 1;
                break;
            } else if (!strnicmp(a + 1, "W:", 2) && a[3] && strlen(a + 3) < PMAX) {
                strcpy(win, a + 3);
                strip_slash(win);
            } else if (!stricmp(a + 1, "NOWIN") || !stricmp(a + 1, "NO32")) {
                nowin = 1;
            } else if (!stricmp(a + 1, "DOS")) {
                dosdrv = 1;
#ifdef WIN_USB
            } else if (!stricmp(a + 1, "NOSOUND")) {
                no_sound = 1;
#endif
            } else {
                usage();
                return a[1] == '?' ? 0 : 1;
            }
        } else if (!dir[0] && strlen(a) < PMAX) {
            strcpy(dir, a);
        } else {
            usage();
            return 1;
        }
    }
    if (windows_running()) {
        printf("Please exit Windows (or the task switcher) and run " INSTEXE " from DOS.\n");
        return 2;
    }
    own_dir(src, argv[0]);
    for (i = 0; pkgfiles[i].name; i++) {
        pkg_file(from, src, pkgfiles[i].name);
        if (pkgfiles[i].need && !exists(from)) {
            printf("%s not found. Run " INSTEXE " from the package directory.\n", from);
            return 1;
        }
    }
    if (!nowin) {
        if (win_select(win, 1))
            return 1;
        if (!nwins) {
            printf("No Windows installation supported by " PKG " was found.\n"
                   "Use /W:<windows directory>, or /NOWIN to install the DOS driver only.\n");
            return 1;
        }
    }
    if (!dir[0]) {
        /* default: the newest Windows found */
        pick = -1;
        for (i = 0; i < nwins; i++)
            if (pick < 0 || wins[i].ver > wins[pick].ver)
                pick = i;
        if (pick >= 0 && wins[pick].ver >= WV_30 && !IS9X(&wins[pick]))
            strcpy(dir, wins[pick].dir);
        else
            sprintf(dir, "%c:\\WINDRV", bootdrv);
    }
    if (truename(dir, to)) {
        printf("Invalid directory: %s\n", dir);
        return 1;
    }
    strcpy(dir, to);
    if (strlen(dir) > 60) {
        printf("Directory name too long: %s\n", dir);
        return 1;
    }
    if (make_dir(dir)) {
        printf("Cannot create %s\n", dir);
        return 1;
    }
    same = !stricmp(src, dir);
    inwin = 0;
    for (i = 0; i < nwins; i++)
        if (!stricmp(dir, wins[i].dir))
            inwin = 1;
    printf("Installing to %s\n", dir);
    for (i = 0; !same && pkgfiles[i].name; i++) {
        pkg_file(from, src, pkgfiles[i].name);
        join(to, dir, pkgfiles[i].name);
        if (!exists(from) || (inwin && !stricmp(pkgfiles[i].name, "OEMSETUP.INF")))
            continue;       /* a generic name; do not mix it into the Windows directory */
        if (copy_file(from, to)) {
            printf("  Cannot copy %s to %s\n", from, to);
            return 1;
        }
        copied++;
    }
#ifdef WIN_AUDIO
    if (!same) {
        int k = fw_copy(src, dir);
        if (k < 0)
            return 1;
        if (k)
            printf("  %d firmware files -> %s\\" FWDIR "\n", k, dir);
    }
#endif
    if (!same)
        printf("  %d files copied\n", copied);

    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
#ifdef WIN_UDF
    (void)cfg;
    if (udf_dos_install(dir, useropts ? opts : NULL))
        return 1;
#else
    /* the DOS of Windows 95/98: Windows has the Plug and Play driver, the DOS driver is for
       DOS sessions only (/DOS); Windows ME has no CONFIG.SYS */
    if (dos7() && !dosdrv) {
        printf("MS-DOS 7: " SYSFILE " not added to CONFIG.SYS (Windows has the Plug and Play driver; /DOS adds it for DOS mode)\n");
    } else {
        if (cfg_install(cfg, dir, useropts ? opts : NULL))
            return 1;
#ifdef WIN_CDPKG
        if (cd_install(dir))
            return 1;
#endif
    }
#endif
    for (i = 0; !nowin && i < nwins; i++) {
        printf("%s (%s)\n", wins[i].dir, win_name(&wins[i]));
        if (windows_install(&wins[i], dir) || group_install(&wins[i], dir))
            return 1;
    }
    flush_disks();
    printf("\n" PKG " is installed%s. %s\n"
           "To remove it, run %s\\" UNINEXE ".\n",
           warnings ? " (see the warnings above)" : "",
#ifdef WIN_UDF
           "Restart the computer to load it.",
#else
           win9x_box ? "Install the driver from Device Manager (see above)." : "Restart the computer to load it.",
#endif
           dir);
    return 0;
}

#else /* UNINST */

static void usage(void)
{
    printf("Usage: " UNINEXE " [/K] [/W:windir]\n\n"
           "  /K         keep the files, change only CONFIG.SYS"
#ifdef WIN_USB
           ", SYSTEM.INI and WIN.INI\n"
#elif defined(WIN_AUDIO)
           ", SYSTEM.INI and MIDIMAP.CFG\n"
#else
           " and SYSTEM.INI\n"
#endif
           "  /W:windir  only this Windows (default: every supported Windows found)\n");
}

int main(int argc, char **argv)
{
    char dir[PMAX] = "", own[PMAX], win[PMAX] = "", f[PMAX], cfg[PMAX];
    static const char *const syss[] = { "WINDISK.SYS", "WINUSB.SYS", "WINAUDIO.SYS", "WINUDF.EXE" };
    int i, keep = 0, n = 0, kept = 0, inwin, others = 0;

    bootdrv = boot_drive();
    _harderr(critical_error);
    printf(PKG " uninstall\n\n");
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && !stricmp(a + 1, "K"))
            keep = 1;
        else if ((a[0] == '/' || a[0] == '-') && !strnicmp(a + 1, "W:", 2) && a[3] && strlen(a + 3) < PMAX) {
            strcpy(win, a + 3);
            strip_slash(win);
        }
        else {
            usage();
            return (a[1] == '?') ? 0 : 1;
        }
    }
    if (windows_running()) {
        printf("Please exit Windows (or the task switcher) and run " UNINEXE " from DOS.\n");
        return 2;
    }
    if (win[0]) {
        if (!win_probe(win, &wins[0])) {
            printf("No Windows installation in %s\n", win);
            return 1;
        }
        nwins = 1;
    } else {
        win_scan();
    }
    sprintf(cfg, "%c:\\CONFIG.SYS", bootdrv);
#ifdef WIN_UDF
    (void)cfg;
    if (udf_dos_remove(dir))
        return 1;
#else
    if (cfg_remove(cfg, dir))
        return 1;
#endif
#ifdef WIN_CDPKG
    if (cd_remove(dir))
        return 1;
#endif
    for (i = 0; i < nwins; i++) {
        if (wins[i].ver < WV_30)
            continue;
        printf("%s (%s)\n", wins[i].dir, win_name(&wins[i]));
        if (windows_remove(&wins[i]) || group_remove(&wins[i]))
            return 1;
    }
    if (!dir[0]) {
        own_dir(own, argv[0]);
        join(f, own, SYSFILE);
        if (exists(f))
            strcpy(dir, own);
    }
    inwin = 0;
    for (i = 0; i < nwins; i++)
        if (!stricmp(dir, wins[i].dir))
            inwin = 1;
    if (!keep && dir[0]) {
        printf("Removing files from %s\n", dir);
        /* files shared with another package stay while that package is installed here */
        for (i = 0; i < 4; i++) {
            join(f, dir, syss[i]);
            if (stricmp(syss[i], SYSFILE) && exists(f))
                others |= 1 << i;
        }
        for (i = 0; pkgfiles[i].name; i++) {
            const char *nm = pkgfiles[i].name;
            join(f, dir, nm);
            if (inwin && !stricmp(nm, "OEMSETUP.INF"))
                continue;
            if (pkgfiles[i].pkgs & others) {
                if (exists(f))
                    kept++;
                continue;
            }
            switch (del_file(f)) {
            case 0:
                n++;
                break;
            case -1:
                warn("cannot delete %s", f);
                break;
            }
        }
        printf("  %d files deleted%s\n", n, kept ? " (files shared with another WINDRV package kept)" : "");
#ifdef WIN_AUDIO
        n += fw_remove(dir);
#endif
        if (!inwin && rmdir(dir) == 0)
            printf("  %s removed\n", dir);
    }
    flush_disks();
    printf("\n" PKG " is uninstalled%s.\n"
           "It stays in memory until the computer is restarted.\n",
           warnings ? " (see the warnings above)" : "");
    return 0;
}
#endif
