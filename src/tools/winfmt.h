/* winfmt.h - text formatting of WINCORE API data (shared by USBTREE.EXE and WININFO.EXE) */
#ifndef WINFMT_H
#define WINFMT_H

#include "winapi.h"

struct sbuf {
    char *p;
    unsigned left;
    unsigned len;
};

void sb_init(struct sbuf *s, char *buf, unsigned size);
void sb_printf(struct sbuf *s, const char *fmt, ...);

const char *win_speed(unsigned char s);
const char *win_hctype(unsigned char t);
const char *win_driver(unsigned char d);
const char *win_class(unsigned char cls, unsigned char sub, unsigned char proto);
const char *win_portstate(unsigned long st);

void win_node_line(struct sbuf *s, const struct winusbnode *n);
void win_node_detail(struct sbuf *s, const struct winusbnode *n);
void win_descriptors(struct sbuf *s, const unsigned char *blob, unsigned len);
void win_disks(struct sbuf *s, const struct windiskinfo *d, int n, int firstdrive);
void win_info(struct sbuf *s, const struct wininfo *in);

#endif
