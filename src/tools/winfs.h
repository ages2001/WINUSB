/* winfs.h - partition table and file system recognition for USBTREE.EXE and WININFO.EXE */
#ifndef WINFS_H
#define WINFS_H

#include "winfmt.h"

/* reads cnt device sectors starting at lba into buf (size bytes); 0 = success */
typedef int (*fs_readfn)(void *ctx, unsigned long lba, unsigned cnt, unsigned char *buf, unsigned size);

struct fsdisk {
    fs_readfn      read;
    void          *ctx;
    unsigned long  ssize;       /* device sector size */
    unsigned long  sectors;     /* device sectors (0xFFFFFFFF when larger) */
    int            index;       /* core disk index, for the drive letters */
    const struct winunit *units;
    int            nunits;
    int            firstdrive;
};

/* partition table and the file system of every partition */
void win_partitions(struct sbuf *s, const struct fsdisk *d);

/* file system at device sector start (nsect sectors): name and version into out, volume label into label */
int win_fsdetect(const struct fsdisk *d, unsigned long start, unsigned long nsect, char *out, int outmax,
                 char *label, int lblmax);

/* "7.45 GB" style size of n sectors of ssize bytes */
void win_size(char *out, unsigned long n, unsigned long hi, unsigned long ssize);

#endif
