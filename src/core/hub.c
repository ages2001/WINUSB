/* hub.c - external USB 2.0 / USB 3.x hub class driver */
#include "usb.h"

struct hubpriv {
    struct usb_hub *h;
    struct usb_pipe intp;
    struct usb_xfer intx;
    u8 *intbuf;
    u16 change[MAXPORTS + 1];
    u16 raw[MAXPORTS + 1];
};

static struct hubpriv *hp_of(struct usb_hub *h) { return h->priv; }

static int ext_nports(struct usb_hub *h) { return h->nports; }

static u32 ext_status(struct usb_hub *h, int port)
{
    struct hubpriv *hp = hp_of(h);
    u8 b[4];
    u16 s, c;
    u32 r = 0;
    if (h->dev->gone) return 0xFFFFFFFFUL;
    if (usb_control(h->dev, 0xA3, 0x00, 0, (u16)port, b, 4) < 4) return 0xFFFFFFFFUL;
    s = (u16)(b[0] | (b[1] << 8));
    c = (u16)(b[2] | (b[3] << 8));
    hp->raw[port] = s;
    hp->change[port] = c;
    if (s & 0x0001) r |= PS_CONNECT;
    if (s & 0x0002) r |= PS_ENABLE;
    if (s & 0x0008) r |= PS_OVERCUR;
    if (s & 0x0010) r |= PS_RESET;
    if (c & 0x0001) r |= PS_CCHANGE;
    if (c & ~0x0001) r |= PS_OTHERCHG;
    if (h->super) {
        if (s & 0x0200) r |= PS_POWER;
        r |= (u32)USB_SS << PS_SPEEDSH;
    } else {
        if (s & 0x0100) r |= PS_POWER;
        if (s & 0x0200) r |= (u32)USB_LS << PS_SPEEDSH;
        else if (s & 0x0400) r |= (u32)USB_HS << PS_SPEEDSH;
        else r |= (u32)USB_FS << PS_SPEEDSH;
    }
    return r;
}

static void clear_feat(struct usb_hub *h, int port, u16 f)
{
    usb_control(h->dev, 0x23, 0x01, f, (u16)port, NULL, 0);
}

static void ext_clear(struct usb_hub *h, int port, u32 st)
{
    struct hubpriv *hp = hp_of(h);
    u16 c = hp->change[port];
    (void)st;
    if (c & 0x0001) clear_feat(h, port, 16);    /* C_PORT_CONNECTION */
    if (c & 0x0002) clear_feat(h, port, 17);    /* C_PORT_ENABLE */
    if (c & 0x0004) clear_feat(h, port, 18);    /* C_PORT_SUSPEND */
    if (c & 0x0008) clear_feat(h, port, 19);    /* C_PORT_OVER_CURRENT */
    if (c & 0x0010) clear_feat(h, port, 20);    /* C_PORT_RESET */
    if (h->super) {
        if (c & 0x0020) clear_feat(h, port, 29);    /* C_BH_PORT_RESET */
        if (c & 0x0040) clear_feat(h, port, 25);    /* C_PORT_LINK_STATE */
        if (c & 0x0080) clear_feat(h, port, 26);    /* C_PORT_CONFIG_ERROR */
    }
    hp->change[port] = 0;
}

static int ext_reset(struct usb_hub *h, int port)
{
    struct hubpriv *hp = hp_of(h);
    u32 t0, st;
    if (h->super) {
        /* SuperSpeed ports train by themselves; reset only if not enabled */
        st = ext_status(h, port);
        if ((st & PS_ENABLE) && (st & PS_CONNECT)) return 0;
    }
    usb_control(h->dev, 0x23, 0x03, 4, (u16)port, NULL, 0);     /* SET_FEATURE(PORT_RESET) */
    t0 = plat_ms();
    for (;;) {
        mdelay(10);
        st = ext_status(h, port);
        if (st == 0xFFFFFFFFUL) return -1;
        if (hp->change[port] & 0x0010) break;
        if (!(st & PS_RESET) && (st & PS_ENABLE)) break;
        if (plat_ms() - t0 > 500) return -1;
    }
    ext_clear(h, port, st);
    mdelay(20);
    st = ext_status(h, port);
    if (hp->change[port]) ext_clear(h, port, st);
    return (st & PS_ENABLE) ? 0 : -1;
}

static void ext_disable(struct usb_hub *h, int port)
{
    if (!h->super) clear_feat(h, port, 1);
}

static struct usb_hubops ext_ops = { ext_nports, ext_status, ext_clear, ext_reset, ext_disable, NULL };

static void hub_int_cb(struct usb_xfer *x)
{
    struct hubpriv *hp = x->priv;
    if (x->status == XS_OK) {
        hp->h->changed = 1;
        usb_submit(x);
    } else if (x->status != XS_CANCEL && x->status != XS_GONE && !x->pipe->dev->gone) {
        hp->h->changed = 1;
        usb_submit(x);
    }
}

static int hub_probe(struct usb_dev *d, u8 *ifc, u8 *end)
{
    struct hubpriv *hp;
    struct usb_hub *h;
    u8 desc[16], *ep;
    int n, i, super = d->speed >= USB_SS;
    if (ifc[5] != 9) return -1;
    if (d->depth >= 6) {
        kprintf("usb: hub nesting too deep\n");
        return -1;
    }
    if (super) usb_control(d, 0x20, 12, (u16)(d->depth - 1), 0, NULL, 0);   /* SET_HUB_DEPTH */
    n = usb_control(d, 0xA0, 0x06, (u16)(super ? 0x2A00 : 0x2900), 0, desc, sizeof(desc));
    if (n < 7) {
        kprintf("usb: hub descriptor failed (%d)\n", n);
        return -1;
    }
    ep = usb_find_desc(ifc + ifc[0], end, 5);
    if (!ep) return -1;
    hp = kmalloc(sizeof(*hp));
    h = usb_new_hub(d->hc, d, &ext_ops, desc[2]);
    h->priv = hp;
    hp->h = h;
    h->super = super;
    h->ttthink = (desc[3] >> 5) & 3;
    h->mtt = d->ddesc[6] == 2;
    h->pwrgood_ms = desc[5] * 2;
    d->driver = UD_HUB;
    if (d->hc->ops->dev_update) d->hc->ops->dev_update(d->hc, d);
    if (usb_pipe_open(d, &hp->intp, ep) == 0) {
        hp->intbuf = kmalloc(8);
        hp->intx.pipe = &hp->intp;
        hp->intx.buf = hp->intbuf;
        hp->intx.len = (h->nports + 8) / 8;
        hp->intx.in = 1;
        hp->intx.cb = hub_int_cb;
        hp->intx.priv = hp;
        usb_submit(&hp->intx);
    }
    for (i = 1; i <= h->nports; i++)
        usb_control(d, 0x23, 0x03, 8, (u16)i, NULL, 0);         /* SET_FEATURE(PORT_POWER) */
    mdelay(MAX(h->pwrgood_ms, 100));
    h->changed = 1;
    kprintf("usb: %s hub with %d ports%s\n", super ? "USB3" : (d->speed == USB_HS ? "USB2" : "USB1.1"),
            h->nports, h->mtt ? ", multi-TT" : "");
    return 0;
}

static void hub_detach(struct usb_dev *d)
{
    struct usb_hub *h = d->ownhub;
    struct hubpriv *hp;
    if (!h) return;
    hp = h->priv;
    usb_cancel(&hp->intx);
    usb_pipe_close(&hp->intp);
    if (hp->intbuf) kfree(hp->intbuf);
    kfree(hp);
    h->priv = NULL;
}

struct usb_class hub_class = { "hub", hub_probe, hub_detach, NULL };
