/* dsvxd.c - the DirectSound HAL of WINAUDIO.VXD and WINUSB.VXD (Windows 95/98/ME with
   DirectX 3 or later).

   Every sound device with an output registers an IDsDriver object with DSOUND.VXD
   (DSOUND_RegisterDeviceDriver): those on a PCI controller in WINAUDIO.VXD, the USB Audio
   devices in WINUSB.VXD. The primary buffer is a hardware buffer: DirectSound's kernel mixer
   mixes the software buffers of the programs into it ahead of the write cursor, and the
   sound engine plays it round and round (aeng_ds_*). The secondary buffers are hardware
   buffers too, up to MAXSEC of them on a device: the sound engine mixes them like the wave
   streams, each at its own rate, volume and pan, looping or once; DirectSound mixes in
   software only what it cannot place there (3D buffers, more buffers than the hardware has).

   DirectSound finds the wave device of the driver through the devnode: MMSYSTEM answers
   DRV_QUERYDEVNODE with the devnode it loaded WINSOUND.DRV for (DevLoader MMDEVLDR.VXD,
   WINAUDM.INF, WINUSBA.INF; WINSOUND.DRV learns which devices are on which devnode through
   WINF_AUD_DSNODE), the device number counts the wave devices of that devnode.

   DirectSound calls the methods in ring 0 with the stdcall convention of COM. */
#include "pnpint.h"
#include "audio.h"

#define DS_OK                   0x00000000UL
#define E_NOINTERFACE           0x80004002UL
#define DSERR_UNSUPPORTED       0x80004001UL
#define DSERR_GENERIC           0x80004005UL
#define DSERR_INVALIDPARAM      0x80070057UL
#define DSERR_OUTOFMEMORY       0x8007000EUL
#define DSERR_ALLOCATED         0x8878000AUL
#define DSERR_CONTROLUNAVAIL    0x8878001EUL
#define DSERR_INVALIDCALL       0x88780032UL
#define DSERR_BADFORMAT         0x88780064UL
#define DSERR_NODRIVER          0x88780078UL

#define DSDDESC_DONTNEEDPRIMARYLOCK     0x00000008UL
#define DSDDESC_DONTNEEDSECONDARYLOCK   0x00000010UL
#define DSDHEAP_NOHEAP          0
#define DSCAPS_PRIMARYMONO      0x00000001UL
#define DSCAPS_PRIMARYSTEREO    0x00000002UL
#define DSCAPS_PRIMARY8BIT      0x00000004UL
#define DSCAPS_PRIMARY16BIT     0x00000008UL
#define DSCAPS_CONTINUOUSRATE   0x00000010UL
#define DSCAPS_SECONDARYMONO    0x00000100UL
#define DSCAPS_SECONDARYSTEREO  0x00000200UL
#define DSCAPS_SECONDARY8BIT    0x00000400UL
#define DSCAPS_SECONDARY16BIT   0x00000800UL
#define DSBCAPS_PRIMARYBUFFER   0x00000001UL
#define DSBCAPS_CTRL3D          0x00000010UL
#define DSBCAPS_CTRLPAN         0x00000040UL
#define DSBCAPS_CTRLVOLUME      0x00000080UL
#define DSBPLAY_LOOPING         0x00000001UL

#define WAVE_FORMAT_PCM         1
#define DSBUF                   65536UL     /* bytes of a primary buffer: 370 ms at 44.1 kHz stereo */
#define MAXDS                   8
#define MAXSEC                  16          /* hardware secondary buffers of a device */
#define SECMEM                  (8UL << 20) /* their memory (locked), all devices together */
#define KEEPSTREAMS             4           /* engine streams left for the wave devices */
#define MINRATE                 100
#define MAXRATE                 100000UL

#ifdef WIN_USB
#define DS_VXD                  "WINUSB.VXD"
#define DS_TAG                  "WINUSB"
#else
#define DS_VXD                  "WINAUDIO.VXD"
#define DS_TAG                  "WINAUDIO"
#endif

typedef u32 HRESULT;

struct dsdesc {                 /* DSDRIVERDESC */
    u32 flags;
    char desc[256];
    char drvname[256];
    u32 devnode;
    u16 vxdid, rsv;
    u32 devnum;
    u32 heaptype;
    void *ddheap;
    u32 memstart, memend, memextra;
    void *rsv1, *rsv2;
};

struct dscaps {                 /* DSDRIVERCAPS */
    u32 flags, minrate, maxrate, primaries;
    u32 hwmix[6];               /* max all, static, streaming; free all, static, streaming */
    u32 hw3d[6];
    u32 totalmem, freemem, contigmem;
};

struct dsvolpan {               /* DSVOLUMEPAN */
    u32 totalleft, totalright;  /* 16.16, volume and pan together */
    s32 vol;
    u32 volamp;
    s32 pan;
    u32 panleft, panright;
};

struct wfx {                    /* WAVEFORMATEX */
    u16 tag, ch;
    u32 rate, avgbytes;
    u16 align, bits, cbsize;
};

struct guid {
    u32 d1;
    u16 d2, d3;
    u8 d4[8];
};

static const struct guid iid_unknown = { 0x00000000UL, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const struct guid iid_dsdriver = { 0x8C4233C0UL, 0xB4CC, 0x11CE, { 0x92, 0x94, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const struct guid iid_dsbuffer = { 0x8C4233C1UL, 0xB4CC, 0x11CE, { 0x92, 0x94, 0x44, 0x45, 0x53, 0x54, 0, 0 } };

struct dsdrv;

/* the memory of a secondary buffer, shared with its duplicates */
struct dsmem {
    u32 refs;
    u32 handle;                 /* _PageAllocate */
    u32 bytes;                  /* allocated */
    u8 *data;
};

/* a buffer object (IDsDriverBuffer): the primary buffer of a driver or a secondary one */
struct dsbuf {
    const void *vtbl;
    u32 refs;
    struct dsdrv *drv;
    int sid;                    /* engine stream, -1 none */
    u8 playing, primary, rsv[2];
    u32 size;                   /* bytes */
    u8 *data;
    struct dsmem *mem;          /* secondary */
    struct wfx fmt;             /* secondary: its format */
    struct winring hdr;         /* secondary: the engine's counters (the data is elsewhere) */
};

/* the driver object of one sound device (IDsDriver) */
struct dsdrv {
    const void *vtbl;
    u32 refs;
    int dev;                    /* engine device */
    u32 loc;                    /* where it is (aeng_ds_loc) */
    u32 devnode, devnum;
    u8 registered, opened, dead;
    struct winring *ring;       /* the primary buffer: header and DSBUF bytes, locked */
    struct dsbuf buf;
    struct dsbuf *sec[MAXSEC];  /* the secondary buffers */
    char name[48];
};

extern u32 __cdecl vxdldr_load(const char *name);           /* 0 failed */
extern u32 __cdecl dsound_register(void *drv, u32 flags);
extern u32 __cdecl dsound_deregister(void *drv, u32 flags);
extern void __cdecl thk_dsappy(void);
extern u32 __cdecl vmm_page_alloc(u32 npages, u32 align, u32 maxphys, u32 flags, u32 *phys, u32 *lin);
extern void __cdecl vmm_page_free(u32 handle);

static struct dsdrv *drvs[MAXDS];
static int dsound_loaded;       /* 1 loaded, -1 not there (no DirectX) */
static u32 secmem;              /* bytes of the secondary buffers */

static int same_iid(const void *a, const struct guid *b)
{
    return a && !memcmp(a, b, sizeof(*b));
}

/* the engine device behind the driver is still the one it was made for */
static int alive(struct dsdrv *d)
{
    return !d->dead && aeng_ds_loc(d->dev) == d->loc;
}

static int nsec(struct dsdrv *d)
{
    int i, n = 0;
    for (i = 0; i < MAXSEC; i++) n += d->sec[i] != NULL;
    return n;
}

/* ---------------------------------------------------------------- secondary buffer memory */

static struct dsmem *mem_new(u32 bytes)
{
    struct dsmem *m;
    u32 pages = (bytes + 4095) / 4096, phys = 0, lin = 0, h;
    if (secmem + pages * 4096 > SECMEM) return NULL;
    m = vmm_heap_alloc(sizeof(*m));
    if (!m) return NULL;
    h = vmm_page_alloc(pages, 0, 0, 0x08, &phys, &lin);
    if (!h || !lin) {
        vmm_heap_free(m);
        return NULL;
    }
    m->refs = 1;
    m->handle = h;
    m->bytes = pages * 4096;
    m->data = (u8 *)lin;
    secmem += m->bytes;
    return m;
}

static void mem_release(struct dsmem *m)
{
    if (!m || --m->refs) return;
    secmem -= m->bytes;
    vmm_page_free(m->handle);
    vmm_heap_free(m);
}

/* ---------------------------------------------------------------- IDsDriverBuffer */

static HRESULT __stdcall b_qi(struct dsbuf *b, const void *iid, void **ppv)
{
    if (!ppv) return DSERR_INVALIDPARAM;
    if (same_iid(iid, &iid_unknown) || same_iid(iid, &iid_dsbuffer)) {
        b->refs++;
        *ppv = b;
        return DS_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static u32 __stdcall b_addref(struct dsbuf *b)
{
    return ++b->refs;
}

static u32 __stdcall b_release(struct dsbuf *b)
{
    struct dsdrv *d = b->drv;
    int i;
    if (!b->refs) return 0;
    if (--b->refs) return b->refs;
    vmm_begin_crit();
    if (b->sid >= 0) {
        if (b->primary) aeng_ds_run(b->sid, 0);
        aeng_ds_close(b->sid);
    }
    vmm_end_crit();
    b->sid = -1;
    b->playing = 0;
    if (b->primary) return 0;
    for (i = 0; i < MAXSEC; i++)
        if (d->sec[i] == b) d->sec[i] = NULL;
    mem_release(b->mem);
    vmm_heap_free(b);
    return 0;
}

static HRESULT __stdcall b_lock(struct dsbuf *b, void **p1, u32 *l1, void **p2, u32 *l2, u32 pos, u32 len, u32 flags)
{
    (void)flags;
    if (!p1 || !l1 || pos >= b->size || len > b->size) return DSERR_INVALIDPARAM;
    *p1 = b->data + pos;
    if (pos + len <= b->size) {
        *l1 = len;
        if (p2) *p2 = NULL;
        if (l2) *l2 = 0;
    } else {
        *l1 = b->size - pos;
        if (p2) *p2 = b->data;
        if (l2) *l2 = len - (b->size - pos);
    }
    return DS_OK;
}

static HRESULT __stdcall b_unlock(struct dsbuf *b, void *p1, u32 l1, void *p2, u32 l2)
{
    (void)b; (void)p1; (void)l1; (void)p2; (void)l2;
    return DS_OK;
}

/* the primary buffer only: a secondary buffer keeps the format it was made with */
static HRESULT __stdcall b_setformat(struct dsbuf *b, const struct wfx *f)
{
    HRESULT r = DS_OK;
    if (!f) return DSERR_INVALIDPARAM;
    if (!b->primary) return DSERR_UNSUPPORTED;
    if (f->tag != WAVE_FORMAT_PCM) return DSERR_BADFORMAT;
    if (b->playing) return DSERR_INVALIDCALL;
    if (!alive(b->drv)) return DSERR_NODRIVER;
    vmm_begin_crit();
    if (b->sid < 0 || aeng_ds_format(b->sid, f->rate, f->bits, f->ch)) r = DSERR_BADFORMAT;
    vmm_end_crit();
    return r;
}

static HRESULT __stdcall b_setfreq(struct dsbuf *b, u32 hz)
{
    if (b->primary) return DSERR_UNSUPPORTED;
    if (hz < MINRATE || hz > MAXRATE) return DSERR_INVALIDPARAM;
    if (b->sid < 0) return DSERR_INVALIDCALL;
    vmm_begin_crit();
    aeng_ds_rate(b->sid, hz);
    vmm_end_crit();
    return DS_OK;
}

static HRESULT __stdcall b_setvolpan(struct dsbuf *b, const struct dsvolpan *vp)
{
    if (b->primary) return DSERR_UNSUPPORTED;
    if (!vp) return DSERR_INVALIDPARAM;
    if (b->sid < 0) return DSERR_INVALIDCALL;
    vmm_begin_crit();
    aeng_ds_vol(b->sid, vp->totalleft, vp->totalright);
    vmm_end_crit();
    return DS_OK;
}

static HRESULT __stdcall b_setpos(struct dsbuf *b, u32 pos)
{
    if (b->primary) return DSERR_UNSUPPORTED;
    if (pos >= b->size) return DSERR_INVALIDPARAM;
    if (b->sid < 0) return DSERR_INVALIDCALL;
    vmm_begin_crit();
    aeng_ds_setpos(b->sid, pos);
    vmm_end_crit();
    return DS_OK;
}

/* a secondary buffer played once to its end is at its size: DirectSound then stops it */
static HRESULT __stdcall b_getpos(struct dsbuf *b, u32 *play, u32 *write)
{
    u32 p, w;
    if (b->sid < 0) return DSERR_INVALIDCALL;
    aeng_ds_pos(b->sid, &p, &w);
    if (play) *play = p;
    if (write) *write = w;
    return DS_OK;
}

static HRESULT __stdcall b_play(struct dsbuf *b, u32 rsv1, u32 rsv2, u32 flags)
{
    (void)rsv1; (void)rsv2;
    if (b->sid < 0) return DSERR_INVALIDCALL;
    if (!alive(b->drv)) return DSERR_NODRIVER;
    vmm_begin_crit();
    if (b->primary) aeng_ds_run(b->sid, 1);     /* the primary buffer always loops */
    else aeng_ds_play(b->sid, (flags & DSBPLAY_LOOPING) != 0);
    vmm_end_crit();
    b->playing = 1;
    return DS_OK;
}

static HRESULT __stdcall b_stop(struct dsbuf *b)
{
    if (b->sid < 0) return DSERR_INVALIDCALL;
    vmm_begin_crit();
    if (b->primary) aeng_ds_run(b->sid, 0);
    else aeng_ds_stop(b->sid);
    vmm_end_crit();
    b->playing = 0;
    return DS_OK;
}

static const void *const buf_vtbl[] = {
    (void *)b_qi, (void *)b_addref, (void *)b_release, (void *)b_lock, (void *)b_unlock,
    (void *)b_setformat, (void *)b_setfreq, (void *)b_setvolpan, (void *)b_setpos, (void *)b_getpos,
    (void *)b_play, (void *)b_stop
};

/* ---------------------------------------------------------------- IDsDriver */

static HRESULT __stdcall d_qi(struct dsdrv *d, const void *iid, void **ppv)
{
    if (!ppv) return DSERR_INVALIDPARAM;
    if (same_iid(iid, &iid_unknown) || same_iid(iid, &iid_dsdriver)) {
        d->refs++;
        *ppv = d;
        return DS_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static u32 __stdcall d_addref(struct dsdrv *d)
{
    return ++d->refs;
}

/* the object stays: DSOUND.VXD may hold it until it has seen the deregistration */
static u32 __stdcall d_release(struct dsdrv *d)
{
    if (d->refs) d->refs--;
    return d->refs;
}

static HRESULT __stdcall d_getdesc(struct dsdrv *d, struct dsdesc *o)
{
    if (!o) return DSERR_INVALIDPARAM;
    memset(o, 0, sizeof(*o));
    o->flags = DSDDESC_DONTNEEDPRIMARYLOCK | DSDDESC_DONTNEEDSECONDARYLOCK;
    ksnprintf(o->desc, sizeof(o->desc), "%s (" DS_TAG ")", d->name);
    strlcpy(o->drvname, DS_VXD, sizeof(o->drvname));
    o->devnode = d->devnode;
    o->vxdid = WIN_VXD_ID;
    o->devnum = d->devnum;
    o->heaptype = DSDHEAP_NOHEAP;
    return DS_OK;
}

static HRESULT __stdcall d_open(struct dsdrv *d)
{
    if (!alive(d)) return DSERR_NODRIVER;
    d->opened = 1;
    return DS_OK;
}

static HRESULT __stdcall d_close(struct dsdrv *d)
{
    d->opened = 0;
    return DS_OK;
}

/* secondary buffers the device can take now */
static u32 sec_free(struct dsdrv *d)
{
    int f;
    vmm_begin_crit();
    f = aeng_ds_free() - KEEPSTREAMS;
    vmm_end_crit();
    if (f > MAXSEC - nsec(d)) f = MAXSEC - nsec(d);
    return f > 0 ? (u32)f : 0;
}

static HRESULT __stdcall d_getcaps(struct dsdrv *d, struct dscaps *c)
{
    u32 f;
    if (!c) return DSERR_INVALIDPARAM;
    memset(c, 0, sizeof(*c));
    c->flags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
               DSCAPS_CONTINUOUSRATE | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO |
               DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    c->minrate = MINRATE;
    c->maxrate = MAXRATE;
    c->primaries = 1;
    f = sec_free(d);
    c->hwmix[0] = c->hwmix[1] = c->hwmix[2] = MAXSEC;
    c->hwmix[3] = c->hwmix[4] = c->hwmix[5] = f;
    return DS_OK;
}

/* a secondary buffer object on memory m (a new buffer or a duplicate) */
static HRESULT sec_new(struct dsdrv *d, struct dsmem *m, u32 size, const struct wfx *f, struct dsbuf **out)
{
    struct dsbuf *b;
    int i, sid = -1;
    for (i = 0; i < MAXSEC && d->sec[i]; i++) ;
    if (i == MAXSEC || !sec_free(d)) return DSERR_ALLOCATED;
    b = vmm_heap_alloc(sizeof(*b));
    if (!b) return DSERR_OUTOFMEMORY;
    memset(b, 0, sizeof(*b));
    b->hdr.magic = WINRING_MAGIC;
    b->hdr.size = size;
    vmm_begin_crit();
    if (aeng_ds_free() > KEEPSTREAMS) sid = aeng_ds_open(d->dev, &b->hdr, m->data, f->rate, f->bits, f->ch, 1);
    vmm_end_crit();
    if (sid < 0) {
        vmm_heap_free(b);
        return DSERR_ALLOCATED;
    }
    m->refs++;
    b->vtbl = buf_vtbl;
    b->refs = 1;
    b->drv = d;
    b->sid = sid;
    b->size = size;
    b->data = m->data;
    b->mem = m;
    b->fmt = *f;
    d->sec[i] = b;
    *out = b;
    return DS_OK;
}

static HRESULT __stdcall d_create(struct dsdrv *d, const struct wfx *f, u32 flags, u32 cardaddr, u32 *size,
                                  u8 **data, void **ppv)
{
    struct dsbuf *b = &d->buf;
    struct dsmem *m;
    HRESULT r;
    int sid;
    (void)cardaddr;
    if (ppv) *ppv = NULL;
    if (!f || !size || !data || !ppv) return DSERR_INVALIDPARAM;
    if (f->tag != WAVE_FORMAT_PCM) return DSERR_BADFORMAT;
    if (!alive(d)) return DSERR_NODRIVER;
    if (!(flags & DSBCAPS_PRIMARYBUFFER)) {
        u32 fsz = (u32)f->bits / 8 * f->ch;
        if (flags & DSBCAPS_CTRL3D) return DSERR_CONTROLUNAVAIL;     /* DirectSound does 3D in software */
        if ((f->bits != 8 && f->bits != 16) || (f->ch != 1 && f->ch != 2) || f->rate < MINRATE ||
            f->rate > MAXRATE)
            return DSERR_BADFORMAT;
        if (!*size || *size % fsz || *size > SECMEM) return DSERR_INVALIDPARAM;
        if (!(m = mem_new(*size))) return DSERR_OUTOFMEMORY;
        memset(m->data, f->bits == 8 ? 0x80 : 0, *size);
        r = sec_new(d, m, *size, f, &b);
        mem_release(m);                         /* the buffer holds it now */
        if (r != DS_OK) return r;
        *data = b->data;
        *ppv = b;
        return DS_OK;
    }
    if (flags & (DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    if (b->refs) return DSERR_ALLOCATED;
    vmm_begin_crit();
    memset((u8 *)d->ring + 32, f->bits == 8 ? 0x80 : 0, DSBUF);
    sid = aeng_ds_open(d->dev, d->ring, (u8 *)d->ring + 32, f->rate, f->bits, f->ch, 0);
    vmm_end_crit();
    if (sid < 0) return DSERR_BADFORMAT;
    b->vtbl = buf_vtbl;
    b->refs = 1;
    b->drv = d;
    b->sid = sid;
    b->playing = 0;
    b->primary = 1;
    b->size = DSBUF;
    b->data = (u8 *)d->ring + 32;
    *size = DSBUF;
    *data = b->data;
    *ppv = b;
    return DS_OK;
}

/* a secondary buffer on the memory of another, playing on its own */
static HRESULT __stdcall d_duplicate(struct dsdrv *d, struct dsbuf *src, void **ppv)
{
    struct dsbuf *b;
    HRESULT r;
    if (ppv) *ppv = NULL;
    if (!src || !ppv) return DSERR_INVALIDPARAM;
    if (src->primary || src->drv != d || !src->mem) return DSERR_INVALIDCALL;
    if (!alive(d)) return DSERR_NODRIVER;
    r = sec_new(d, src->mem, src->size, &src->fmt, &b);
    if (r == DS_OK) *ppv = b;
    return r;
}

static const void *const drv_vtbl[] = {
    (void *)d_qi, (void *)d_addref, (void *)d_release, (void *)d_getdesc, (void *)d_open, (void *)d_close,
    (void *)d_getcaps, (void *)d_create, (void *)d_duplicate
};

/* ---------------------------------------------------------------- registration */

/* the devnode of the sound device at loc (aeng_ds_loc): the HD Audio codec's child devnode,
   the first child of another bus (Intel audio DSP), else the controller's (AC'97); the USB
   device's devnode */
static struct pnode *node_of(u32 loc)
{
    struct pnode *p, *first = NULL;
    int i;
    if (loc & DSLOC_USB) {
#ifdef WIN_USB
        p = pnp_node_by_usbid((u16)loc);
        return p && p->dn ? p : NULL;
#else
        return NULL;
#endif
    }
    p = (loc & 0xFFFF) == 0xFFFF ? NULL : pnp_node_by_bdf(loc & 0xFFFF);
    if (!p || !p->dn) return NULL;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *c = &pnp_nodes[i];
        if (!c->dn || c->parent != p->dn || c->kind != PK_CHILD) continue;
        if ((loc >> 16) && c->key == ((loc & 0xFFFF) << 8 | ((loc >> 16) - 1))) return c;
        if (!first) first = c;
    }
    return first ? first : p;
}

/* where a device is for WINSOUND.DRV: the USB devices with MIDI only too */
static u32 any_loc(int dev)
{
    struct adev *a = adev_get(dev);
    if (a && !a->gone && !a->hidden && a->usbid) return DSLOC_USB | a->usbid;
    return aeng_ds_loc(dev);
}

/* the devnode of an engine device and its number among the output devices of that devnode */
static u32 devnode_of(int dev, u32 *num, u8 *mm)
{
    struct pnode *p;
    u32 loc;
    int i;
    *num = 0;
    *mm = 0;
    vmm_begin_crit();
    loc = any_loc(dev);
    p = node_of(loc);
    for (i = 0; p && i < dev; i++)
        if (node_of(aeng_ds_loc(i)) == p) (*num)++;
    vmm_end_crit();
    if (!p) return 0;
    *mm = p->mm;
    return p->dn;
}

/* WINF_AUD_DSNODE for WINSOUND.DRV: arg0 device -> retlen devnode, arg1 1 when MMDEVLDR.VXD
   loaded it (MMSYSTEM loads the driver for that devnode: the device belongs to that instance);
   arg0 0xFFFFFFFF: WINE_OK (the devices get devnodes) */
int ds_api_devnode(struct winapireq *r)
{
    u32 num, dn;
    u8 mm;
    if (r->arg0 == 0xFFFFFFFFUL) return WINE_OK;
    dn = devnode_of((int)(r->arg0 & 0xFF), &num, &mm);
    if (!dn) return WINE_NODEV;
    r->retlen = dn;
    r->arg1 = mm;
    return WINE_OK;
}

static struct dsdrv *drv_new(void)
{
    struct dsdrv *d;
    u32 phys = 0, lin = 0;
    int i;
    for (i = 0; i < MAXDS; i++)
        if (drvs[i] && drvs[i]->dead && !drvs[i]->registered && !drvs[i]->buf.refs && !nsec(drvs[i])) return drvs[i];
    for (i = 0; i < MAXDS && drvs[i]; i++) ;
    if (i == MAXDS) return NULL;
    d = vmm_heap_alloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    /* the header just before a page boundary, the buffer page aligned after it */
    if (!vmm_page_alloc(DSBUF / 4096 + 1, 0, 0, 0x08, &phys, &lin) || !lin) {
        vmm_heap_free(d);
        return NULL;
    }
    d->ring = (struct winring *)(lin + 4096 - 32);
    drvs[i] = d;
    return d;
}

/* every output device on a devnode gets a driver; the ones whose device went away are
   deregistered (appy time: DSOUND.VXD is loaded from disk the first time) */
void __cdecl vxd_dsappy(u32 ref)
{
    int i, k;
    (void)ref;
    if (dsound_loaded < 0 || pnp_shutting_down()) return;
    for (k = 0; k < MAXDS; k++) {
        struct dsdrv *d = drvs[k];
        if (d && d->registered && !alive(d)) {
            d->dead = 1;
            dsound_deregister(d, 0);
            d->registered = 0;
            kprintf("ds: %s deregistered\n", d->name);
        }
    }
    for (i = 0; i < MAXADEV; i++) {
        struct dsdrv *d;
        struct adev *a;
        u32 num, loc, dn, r;
        u8 mm;
        dn = devnode_of(i, &num, &mm);
        vmm_begin_crit();
        loc = aeng_ds_loc(i);
        a = adev_get(i);
        vmm_end_crit();
        if (!dn || loc == 0xFFFF) continue;
        for (k = 0; k < MAXDS; k++)
            if (drvs[k] && drvs[k]->registered && drvs[k]->dev == i && drvs[k]->loc == loc && drvs[k]->devnum == num) break;
        if (k < MAXDS) continue;
        if (!dsound_loaded) {
            if (!vxdldr_load("DSOUND.VXD")) {
                dsound_loaded = -1;
                kprintf("ds: no DSOUND.VXD (DirectX not installed): DirectSound emulates through the wave driver\n");
                return;
            }
            dsound_loaded = 1;
        }
        d = drv_new();
        if (!d) return;
        d->vtbl = drv_vtbl;
        d->refs = 0;
        d->dev = i;
        d->loc = loc;
        d->devnode = dn;
        d->devnum = num;
        d->dead = 0;
        d->opened = 0;
        d->buf.sid = -1;
        d->buf.refs = 0;
        strlcpy(d->name, a ? a->name : DS_TAG, sizeof(d->name));
        d->ring->magic = WINRING_MAGIC;
        d->ring->size = DSBUF;
        r = dsound_register(d, 0);
        if (r != DS_OK) {
            d->dead = 1;
            kprintf("ds: %s: DSOUND_RegisterDeviceDriver failed (%lx)\n", d->name, r);
            continue;
        }
        d->registered = 1;
        kprintf("ds: %s registered (devnode %08lx, device %lu)\n", d->name, dn, num);
    }
}

/* after the controllers started (and again after a resume) */
void ds_schedule(void)
{
    if (dsound_loaded >= 0) cm_call_at_appy_time((void *)thk_dsappy, 0, 0);
}

/* CONFIG_STOP / REMOVE / SHUTDOWN of a controller (0xFFFFFFFF: all; the USB devices go
   with any controller of WINUSB): first of all, so that DirectSound lets go of the drivers
   while the devices are still there */
void ds_stop(u32 bdf)
{
    int k, i;
    for (k = 0; k < MAXDS; k++) {
        struct dsdrv *d = drvs[k];
        if (!d || !d->registered) continue;
        if (bdf != 0xFFFFFFFFUL && !(d->loc & DSLOC_USB) && (d->loc & 0xFFFF) != bdf) continue;
        vmm_begin_crit();
        if (d->buf.refs && d->buf.sid >= 0) aeng_ds_run(d->buf.sid, 0);
        for (i = 0; i < MAXSEC; i++)
            if (d->sec[i] && d->sec[i]->sid >= 0) aeng_ds_stop(d->sec[i]->sid);
        vmm_end_crit();
        d->buf.playing = 0;
        for (i = 0; i < MAXSEC; i++)
            if (d->sec[i]) d->sec[i]->playing = 0;
        d->dead = 1;
        dsound_deregister(d, 0);
        d->registered = 0;
        kprintf("ds: %s deregistered\n", d->name);
    }
}
