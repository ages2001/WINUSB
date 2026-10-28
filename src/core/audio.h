/* audio.h - WIN sound engine: devices (HDA, AC'97, USB Audio) and streams.
   A stream lives in a ring buffer in conventional memory of the system VM, shared with
   WINSOUND.DRV (struct winring in winapi.h). The engine mixes all playback streams of a
   device into the device's hardware ring and converts its capture ring into the format
   of each recording stream. */
#ifndef AUDIO_H
#define AUDIO_H
#include "core.h"

struct adev;

struct adevops {
    int (*start)(struct adev *a, int dir);          /* hardware ring of out (0) / in (1) running */
    void (*stop)(struct adev *a, int dir);
    u32 (*pos)(struct adev *a, int dir);            /* DMA position, bytes into the ring */
    void (*set_vol)(struct adev *a, int ctl, u32 lr);   /* MX_*: 0..65535 left | right << 16 */
    void (*set_src)(struct adev *a, int src);       /* AS_* input source */
    void (*poll)(struct adev *a);                   /* periodic work (jack sense, USB) */
    void (*shutdown)(struct adev *a);
    void (*midi)(struct adev *a, const u8 *msg, int len);   /* USB MIDI out */
};

struct ahw {                    /* one direction: hardware ring, 16-bit stereo */
    u8 *buf;
    u32 size;                   /* bytes, multiple of the period */
    u32 period;                 /* bytes per period */
    u32 rate;
    u32 fill;                   /* out: bytes written ahead (total); in: bytes taken (total) */
    u32 hwtotal;                /* bytes the device has played / captured (total) */
    u32 lastpos;
    u8 running;
    u8 present;
    u8 users;                   /* open streams */
    u8 noflush;                 /* DMA is cache coherent */
    u32 idle;                   /* plat_ms() when it became idle, 0 busy */
    u32 late;                   /* times the device played data we had not written yet */
    u32 zeroed;                 /* out: silence written up to here (total), ahead of fill */
    u32 latms;                  /* out: how far ahead to mix (ms), 0: the default of the platform */
};

struct adev {
    struct adevops *ops;
    void *priv;
    int index;                  /* device number seen by WINSOUND.DRV */
    u8 type;                    /* AT_* */
    u8 caps;                    /* WINAC_* */
    u8 insrcs;                  /* AS_* bits available */
    u8 insrc;
    u16 vid, pid;
    char name[48];
    struct ahw out, in;
    u32 vol[MX_COUNT];          /* MX_*: left | right << 16 */
    u8 hwvol;                   /* MX_MASTER is done by the hardware */
    u8 mute;                    /* MX_MUTE: master muted */
    u8 gone;                    /* removed (USB): streams stay, nothing is played */
    u8 midiout;                 /* USB MIDI */
    u8 irq;                     /* the controller's interrupt line (0xFF none) */
    u16 pcivid, pcidid;         /* the controller */
    u16 loc;                    /* PCI bus << 8 | device << 3 | function, 0xFFFF none */
    u8 hidden;                  /* internal (the sound bridge to WINUSB): not listed to Windows */
};

#define AT_HDA  1
#define AT_AC97 2
#define AT_USB  3

/* the functions behind a controller for the Plug and Play enumerator of Windows 95/98/ME:
   HD Audio codec function groups, the devices of an Intel audio DSP */
struct afunc {
    u8 kind;                    /* AF_* */
    u8 index;                   /* HD Audio codec address, DSP device number */
    u8 hasdev;                  /* a sound device of the engine plays / records through it */
    u8 rsv;
    u32 vendev;                 /* HD Audio: codec vendor << 16 | device */
    u32 subsys;                 /* HD Audio: subsystem id */
    u32 rev;                    /* HD Audio: revision id */
    char name[48];              /* chip or device name */
};
#define AF_HDA_AUDIO    1       /* HD Audio codec with analog outputs */
#define AF_HDA_HDMI     2       /* HD Audio codec with HDMI / DisplayPort outputs only */
#define AF_HDA_MODEM    3       /* HD Audio modem function group */
#define AF_HDA_OTHER    4       /* other HD Audio audio function group */
#define AF_DSP_DMIC     5       /* Intel DSP: digital microphones */
#define AF_DSP_I2S      6       /* Intel DSP: I2S codec / amplifiers */
#define AF_DSP_SDW      7       /* Intel DSP: SoundWire codec / amplifiers */
#define AF_DSP_HDA      8       /* Intel DSP: HD Audio codec through the DSP */
int audio_functions(u32 bdf, struct afunc *out, int max);  /* bdf 0xFFFFFFFF: the ACPI audio DSP */
int hda_functions(u32 bdf, struct afunc *out, int max);
void codec_name(u32 vendev, char *out, u32 max);           /* "Realtek ALC887" */

int adev_register(struct adev *a);
void adev_unregister(struct adev *a);
struct adev *adev_get(int i);
int adev_count(void);
void aeng_init(void);
void aeng_poll(void);           /* service all devices and streams */
void aeng_shutdown(void);
int aeng_api(struct winapireq *r, u8 *buf);
void aeng_flush_dma(void *p, u32 n);    /* write back CPU caches over a DMA area */

/* internal real-time streams (Sound Blaster emulation) */
int aeng_rt_open(struct winring *r, u32 rate, int volctl);
void aeng_rt_run(int id, int run);
void aeng_rt_rate(int id, u32 rate);
void aeng_rt_vol(int id, u32 vl, u32 vr);
u32 aeng_att_cdb(u32 v);                /* volume 0..65535 (linear amplitude) -> attenuation, 1/100 dB */
void aeng_rt_close(int id);
void aeng_midi(const u8 *m, int len);   /* to the synthesizer on the default device */
void aeng_lowlat(int on);               /* DOS: serviced at the RTC rate */
void aeng_tick(void);
void aeng_bridge_probe(void);           /* WINAUDIO: the sound bridge to a USB Audio device of WINUSB */
const char *aeng_bridge_why(void);     /* WINAUDIO: NULL bridged, else the reason it is not */

/* synthesizer (aeng.c calls it while mixing) */
void synth_init(void);
void synth_render(s32 *acc, int frames, u32 rate, u32 vol);
void synth_midi(const u8 *msg, int len);
void synth_reset(void);
int synth_busy(void);            /* notes sounding or recently played */

/* drivers */
int hda_probe(struct pcidev *p);
int ac97_probe(struct pcidev *p);
void hda_remove(u32 bdf);       /* controller taken away (Plug and Play stop) */
void ac97_remove(u32 bdf);
void aeng_remove(struct adev *a);   /* stop, shut down and unregister a device */

#endif
