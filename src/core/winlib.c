/* winlib.c - minimal C library, logging and DMA pool allocator */
#include "core.h"
#include <stdarg.h>

u32 dbg_level = 1;

void *memset(void *d, int c, u32 n)
{
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, u32 n)
{
    u32 *dw;
    const u32 *sw;
    u8 *db;
    const u8 *sb;
    if ((((u32)d | (u32)s) & 3) == 0) {
        dw = d; sw = s;
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        db = (u8 *)dw; sb = (const u8 *)sw;
    } else {
        db = d; sb = s;
    }
    while (n--) *db++ = *sb++;
    return d;
}

int memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

u32 strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != 0) ;
    return r;
}

void strlcpy(char *d, const char *s, u32 n)
{
    if (!n) return;
    while (--n && *s) *d++ = *s++;
    *d = 0;
}

int strncmp(const char *a, const char *b, u32 n)
{
    while (n--) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

void strlcat(char *d, const char *s, u32 n)
{
    u32 l = strlen(d);
    if (l + 1 >= n) return;
    strlcpy(d + l, s, n - l);
}

void trim_string(char *s)
{
    int n = (int)strlen(s);
    char *p = s;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) s[--n] = 0;
    while (*p == ' ') p++;
    if (p != s) memcpy(s, p, strlen(p) + 1);
}

/* ATA strings are byte-swapped words */
void trim_ata_string(char *dst, const u16 *src, int words)
{
    int i;
    for (i = 0; i < words; i++) {
        dst[i * 2] = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)src[i];
    }
    dst[words * 2] = 0;
    for (i = 0; i < words * 2; i++)
        if ((u8)dst[i] < 0x20 || (u8)dst[i] > 0x7E) dst[i] = ' ';
    trim_string(dst);
}

static u32 crc_tab[256];
u32 crc32(u32 crc, const void *p, u32 n)
{
    const u8 *b = p;
    u32 i, j, c;
    if (!crc_tab[1]) {
        for (i = 0; i < 256; i++) {
            c = i;
            for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- formatted output ---- */
struct pbuf { char *b; u32 n, pos; };

static void pput(struct pbuf *p, char c)
{
    if (p->b) {
        if (p->pos + 1 < p->n) p->b[p->pos] = c;
    } else {
        plat_putc(c);
    }
    p->pos++;
}

static void vfmt(struct pbuf *pb, const char *fmt, va_list ap)
{
    char tmp[16];
    while (*fmt) {
        char c = *fmt++;
        int width = 0, zero = 0, left = 0, i, n, neg;
        u32 v;
        const char *s;
        if (c != '%') { pput(pb, c); continue; }
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') fmt++;
        c = *fmt++;
        n = 0; neg = 0;
        switch (c) {
        case 'd':
        case 'i':
            v = va_arg(ap, u32);
            if ((s32)v < 0) { neg = 1; v = (u32)(-(s32)v); }
            do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            break;
        case 'u':
            v = va_arg(ap, u32);
            do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            break;
        case 'x':
        case 'X':
        case 'p':
            v = va_arg(ap, u32);
            do {
                int d = v & 15;
                tmp[n++] = (char)(d < 10 ? '0' + d : (c == 'x' ? 'a' : 'A') + d - 10);
                v >>= 4;
            } while (v);
            break;
        case 'c':
            tmp[n++] = (char)va_arg(ap, int);
            break;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            i = (int)strlen(s);
            if (!left) while (width-- > i) pput(pb, ' ');
            while (*s) pput(pb, *s++);
            if (left) while (width-- > i) pput(pb, ' ');
            continue;
        default:
            pput(pb, c);
            continue;
        }
        if (neg) {
            if (zero) { pput(pb, '-'); width--; }
            else tmp[n++] = '-';
        }
        if (!left) while (width-- > n) pput(pb, zero ? '0' : ' ');
        while (n) pput(pb, tmp[--n]);
        if (left) while (width-- > 0) pput(pb, ' ');
    }
}

int ksnprintf(char *buf, u32 n, const char *fmt, ...)
{
    struct pbuf pb;
    va_list ap;
    pb.b = buf; pb.n = n; pb.pos = 0;
    va_start(ap, fmt);
    vfmt(&pb, fmt, ap);
    va_end(ap);
    if (n) buf[pb.pos < n ? pb.pos : n - 1] = 0;
    return (int)pb.pos;
}

/* log ring: everything printed with kprintf is kept for USBTREE */
#define LOGSZ 16384
static char logring[LOGSZ];
static u32 logpos;

void log_init(void)
{
    logpos = 0;
}

void log_copy(char *dst, u32 max, u32 *outlen)
{
    u32 n = logpos < LOGSZ ? logpos : LOGSZ;
    u32 start = logpos - n, i;
    if (n > max) { start += n - max; n = max; }
    for (i = 0; i < n; i++) dst[i] = logring[(start + i) % LOGSZ];
    *outlen = n;
}

void kprintf(const char *fmt, ...)
{
    char buf[200];
    struct pbuf pb;
    va_list ap;
    u32 i;
    pb.b = buf; pb.n = sizeof(buf); pb.pos = 0;
    va_start(ap, fmt);
    vfmt(&pb, fmt, ap);
    va_end(ap);
    if (pb.pos >= sizeof(buf)) pb.pos = sizeof(buf) - 1;
    for (i = 0; i < pb.pos; i++) {
        logring[logpos % LOGSZ] = buf[i];
        logpos++;
        if (dbg_level) plat_putc(buf[i]);
    }
}

void udelay(u32 us)
{
    plat_udelay(us);
}

void mdelay(u32 ms)
{
    while (ms--) plat_udelay(1000);
}

/* ---- DMA pool: first-fit allocator with boundary tags ----
   Block header (16 bytes): size (incl. header), flags, prev-free link via free list */
struct pblk {
    u32 size;
    u32 used;
    struct pblk *next;   /* free list */
    u32 rsv;
};

static u32 pool_lin, pool_phys, pool_size;
static struct pblk *freelist;

void pool_init(u32 lin, u32 phys, u32 size)
{
    pool_lin = lin;
    pool_phys = phys;
    pool_size = size & ~15UL;
    freelist = (struct pblk *)lin;
    freelist->size = pool_size;
    freelist->used = 0;
    freelist->next = NULL;
}

u32 vphys(const void *p)
{
    return (u32)p - pool_lin + pool_phys;
}

void *pvirt(u32 phys)
{
    if (phys < pool_phys || phys >= pool_phys + pool_size) return NULL;
    return (void *)(phys - pool_phys + pool_lin);
}

u32 pool_free_bytes(void)
{
    struct pblk *b;
    u32 n = 0;
    for (b = freelist; b; b = b->next) n += b->size;
    return n;
}

static void free_insert(struct pblk *nb)
{
    struct pblk **pp = &freelist, *b;
    /* keep list sorted by address to allow coalescing */
    while (*pp && *pp < nb) pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;
    /* coalesce with next */
    if (nb->next && (u32)nb + nb->size == (u32)nb->next) {
        nb->size += nb->next->size;
        nb->next = nb->next->next;
    }
    /* coalesce with previous */
    for (b = freelist; b && b->next != nb; b = b->next) ;
    if (b && (u32)b + b->size == (u32)nb) {
        b->size += nb->size;
        b->next = nb->next;
    }
}

void *kmalloc_align(u32 size, u32 align)
{
    struct pblk **pp, *b;
    u32 need, f;
    if (align < 16) align = 16;
    size = ALIGN_UP(size, 16);
    f = irq_save();
    for (pp = &freelist; (b = *pp) != NULL; pp = &b->next) {
        u32 data = ALIGN_UP((u32)b + 16, align);
        u32 pad = data - 16 - (u32)b;
        /* if pad is nonzero but too small to hold a free block, move further */
        if (pad && pad < 32) {
            data = ALIGN_UP((u32)b + 16 + 32, align);
            pad = data - 16 - (u32)b;
        }
        /* avoid 4K crossing for small aligned structures (hardware requirement) */
        if (size <= 4096 && align >= 64) {
            u32 ph = vphys((void *)data);
            if ((ph & ~4095UL) != ((ph + size - 1) & ~4095UL)) {
                u32 nd = ALIGN_UP(ph + 1, 4096) - ph + data;
                data = nd;
                pad = data - 16 - (u32)b;
            }
        }
        need = pad + 16 + size;
        if (b->size < need) continue;
        *pp = b->next;
        if (pad) {
            struct pblk *front = b;
            u32 total = b->size;
            front->size = pad;
            front->used = 0;
            b = (struct pblk *)((u32)front + pad);
            b->size = total - pad;
            free_insert(front);
        }
        if (b->size - (16 + size) >= 64) {
            struct pblk *rest = (struct pblk *)((u32)b + 16 + size);
            rest->size = b->size - 16 - size;
            rest->used = 0;
            b->size = 16 + size;
            free_insert(rest);
        }
        b->used = 0x55AA55AA;
        b->next = NULL;
        irq_restore(f);
        memset((u8 *)b + 16, 0, b->size - 16);
        return (u8 *)b + 16;
    }
    irq_restore(f);
    kprintf("win: pool exhausted (%u bytes, align %u)\n", size, align);
    return NULL;
}

void *kmalloc(u32 size)
{
    return kmalloc_align(size, 16);
}

void kfree(void *p)
{
    struct pblk *b;
    u32 f;
    if (!p) return;
    b = (struct pblk *)((u8 *)p - 16);
    if (b->used != 0x55AA55AA) {
        kprintf("win: bad kfree %p\n", p);
        return;
    }
    f = irq_save();
    b->used = 0;
    free_insert(b);
    irq_restore(f);
}
