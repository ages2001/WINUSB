/* camjpeg.h - baseline JPEG decoder for WINCAM (Motion-JPEG camera frames) */
#ifndef CAMJPEG_H
#define CAMJPEG_H

#ifndef FAR
#ifdef __WINDOWS__
#define FAR __far
#else
#define FAR
#endif
#endif

/* input: the caller's buffer, in pieces (a frame in Windows global memory crosses 64K) */
struct jin {
    const unsigned char FAR *p;
    unsigned left;              /* bytes at p */
    unsigned long pos;          /* offset of p in the frame */
    unsigned long size;         /* bytes of the frame */
    void *ctx;
    int ystep, xstep;           /* only every ystep-th row, xstep-th pixel (0: all) */
    /* points p/left to the bytes at 'pos'; returns 0 at the end */
    int (*more)(struct jin *in);
};

/* output: one image row, 3 bytes per pixel in B, G, R order (every xstep-th pixel) */
typedef void (*jrow_fn)(void *ctx, int y, const unsigned char FAR *bgr);

#define JPG_OK          0
#define JPG_BAD         1       /* not a JPEG / damaged */
#define JPG_UNSUP       2       /* progressive, 12-bit, arithmetic, ... */
#define JPG_MEM         3

/* memory of the decoder (default malloc / free); Windows real mode wants fixed blocks */
extern void FAR *(*jpg_alloc)(unsigned long n);
extern void (*jpg_free)(void FAR *p);

int jpg_info(struct jin *in, int *w, int *h);
int jpg_decode(struct jin *in, jrow_fn row, void *ctx);
int jpg_has_dht(struct jin *in);
extern const unsigned char jpg_std_dht[420];

#endif
