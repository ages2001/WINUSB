#!/usr/bin/env python3
"""uredir.py - USB device emulator for QEMU's usb-redir device (usbredir protocol).

    uredir.py <port> <device> [options]     devices: uvc, uvcbulk, uvchs

QEMU connects with:  -chardev socket,id=ur,host=127.0.0.1,port=<port>
                     -device usb-redir,chardev=ur,bus=x.0
(or hot-plugged: QMP chardev-add + device_add). Used to test the WINUSB class drivers
for devices QEMU does not emulate (USB video). Log on stderr."""
import socket, struct, sys, threading, time, io

# ---------------------------------------------------------------- protocol
HELLO, CONNECT, DISCONNECT, RESET, IFINFO, EPINFO = 0, 1, 2, 3, 4, 5
SETCFG, GETCFG, CFGSTATUS, SETALT, GETALT, ALTSTATUS = 6, 7, 8, 9, 10, 11
STARTISO, STOPISO, ISOSTATUS = 12, 13, 14
STARTINT, STOPINT, INTSTATUS = 15, 16, 17
CANCEL = 21
CONTROL, BULK, ISO, INTR = 100, 101, 102, 103
CAP_CONNECT_VERSION, CAP_EP_MPS, CAP_64IDS, CAP_32BULK = 1, 4, 5, 6
MYCAPS = (1 << CAP_CONNECT_VERSION) | (1 << CAP_EP_MPS) | (1 << CAP_64IDS) | (1 << CAP_32BULK)
OK, CANCELLED, INVAL, IOERROR, STALL = 0, 1, 2, 3, 4

VERBOSE = True


def log(*a):
    print("uredir:", *a, file=sys.stderr, flush=True)


class Redir:
    def __init__(self, sock, dev):
        self.s = sock
        self.dev = dev
        dev.redir = self
        self.lock = threading.Lock()
        self.peer_caps = 0
        self.iso_eps = {}           # ep -> running
        self.int_eps = set()
        self.pending = {}           # id -> (ep, length) bulk IN waiting for data
        self.alive = True
        self.ids64 = False
        self.bulk32 = False

    def send(self, typ, pid, hdr, data=b""):
        if self.ids64:
            msg = struct.pack("<IIQ", typ, len(hdr) + len(data), pid) + hdr + data
        else:
            msg = struct.pack("<III", typ, len(hdr) + len(data), pid) + hdr + data
        with self.lock:
            try:
                self.s.sendall(msg)
            except OSError:
                self.alive = False

    def recvn(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b

    def hello(self):
        self.send(HELLO, 0, b"uredir.py 1.0".ljust(64, b"\0"), struct.pack("<I", MYCAPS))

    def send_info(self):
        d = self.dev
        ifs = d.interfaces()
        hdr = struct.pack("<I", len(ifs))
        cols = [bytes(32)] * 4
        a = [bytearray(32) for _ in range(4)]
        for i, (num, cls, sub, proto) in enumerate(ifs):
            a[0][i], a[1][i], a[2][i], a[3][i] = num, cls, sub, proto
        self.send(IFINFO, 0, hdr + b"".join(bytes(x) for x in a))
        typ, iv, itf, mps = bytearray([255] * 32), bytearray(32), bytearray(32), [0] * 32
        typ[0] = typ[16] = 0                # control endpoints
        mps[0] = mps[16] = d.mps0
        for ep, t, interval, iface, m in d.endpoints():
            k = (ep & 0x0F) | (0x10 if ep & 0x80 else 0)
            typ[k], iv[k], itf[k], mps[k] = t, interval, iface, m
        body = bytes(typ) + bytes(iv) + bytes(itf)
        if self.peer_caps & (1 << CAP_EP_MPS):
            body += struct.pack("<32H", *mps)
        self.send(EPINFO, 0, body)

    def connect(self):
        d = self.dev
        dd = d.device_desc()
        speed = d.speed
        hdr = struct.pack("<BBBBHH", speed, dd[4], dd[5], dd[6], d.vid, d.pid)
        if self.peer_caps & (1 << CAP_CONNECT_VERSION):
            hdr += struct.pack("<H", 0x0100)
        self.send_info()
        self.send(CONNECT, 0, hdr)
        log("device connected:", d.__class__.__name__)

    def run(self):
        self.hello()
        threading.Thread(target=self.iso_thread, daemon=True).start()
        while self.alive:
            try:
                if self.ids64:
                    typ, length, pid = struct.unpack("<IIQ", self.recvn(16))
                else:
                    typ, length, pid = struct.unpack("<III", self.recvn(12))
                body = self.recvn(length)
            except (EOFError, OSError):
                break
            self.handle(typ, pid, body)
        self.alive = False
        log("connection closed")

    def handle(self, typ, pid, b):
        d = self.dev
        if typ == HELLO:
            ver = b[:64].rstrip(b"\0").decode(errors="replace")
            self.peer_caps = struct.unpack("<I", b[64:68])[0] if len(b) >= 68 else 0
            log("peer", ver, "caps %x" % self.peer_caps)
            both = self.peer_caps & MYCAPS
            self.ids64 = bool(both & (1 << CAP_64IDS))
            self.bulk32 = bool(both & (1 << CAP_32BULK))
            self.connect()
        elif typ == RESET:
            d.reset()
        elif typ == SETCFG:
            d.config = b[0]
            self.send_info()
            self.send(CFGSTATUS, pid, bytes([OK, d.config]))
        elif typ == GETCFG:
            self.send(CFGSTATUS, pid, bytes([OK, d.config]))
        elif typ == SETALT:
            d.set_alt(b[0], b[1])
            self.send_info()
            self.send(ALTSTATUS, pid, bytes([OK, b[0], b[1]]))
        elif typ == GETALT:
            self.send(ALTSTATUS, pid, bytes([OK, b[0], d.alts.get(b[0], 0)]))
        elif typ == STARTISO:
            self.iso_eps[b[0]] = True
            self.send(ISOSTATUS, pid, bytes([OK, b[0]]))
            log("iso stream on ep %02x, %d pkts/urb, %d urbs" % (b[0], b[1], b[2]))
        elif typ == STOPISO:
            self.iso_eps.pop(b[0], None)
            self.send(ISOSTATUS, pid, bytes([OK, b[0]]))
        elif typ == STARTINT:
            self.int_eps.add(b[0])
            self.send(INTSTATUS, pid, bytes([OK, b[0]]))
        elif typ == STOPINT:
            self.int_eps.discard(b[0])
            self.send(INTSTATUS, pid, bytes([OK, b[0]]))
        elif typ == CANCEL:
            self.pending.pop(pid, None)
        elif typ == CONTROL:
            ep, req, rt, st, val, idx, ln = struct.unpack("<BBBBHHH", b[:10])
            data = b[10:]
            status, out = d.control(rt, req, val, idx, ln, data)
            if VERBOSE:
                log("ctrl %02x %02x %04x %04x %d -> %d, %d bytes" % (rt, req, val, idx, ln, status, len(out)))
            if rt & 0x80:
                out = out[:ln]
                self.send(CONTROL, pid, struct.pack("<BBBBHHH", ep, req, rt, status, val, idx, len(out)), out)
            else:
                self.send(CONTROL, pid, struct.pack("<BBBBHHH", ep, req, rt, status, val, idx, ln))
        elif typ == BULK:
            ep, st, ln, stream = struct.unpack("<BBHI", b[:8])
            hl = 8
            if self.bulk32:
                ln |= struct.unpack("<H", b[8:10])[0] << 16
                hl = 10
            if ep & 0x80:
                self.pending[pid] = (ep, ln)
                self.serve_bulk()
            else:
                d.bulk_out(ep, b[hl:])
                self.send(BULK, pid, self.bulk_hdr(ep, OK, ln))
                self.serve_bulk()
        else:
            log("message type %d ignored" % typ)
        if VERBOSE and typ != CONTROL:
            log("msg %d len %d" % (typ, len(b)))

    def bulk_hdr(self, ep, st, ln):
        h = struct.pack("<BBHI", ep, st, ln & 0xFFFF, 0)
        if self.bulk32:
            h += struct.pack("<H", ln >> 16)
        return h

    def serve_bulk(self):
        for pid, (ep, ln) in list(self.pending.items()):
            data = self.dev.bulk_in(ep, ln)
            if data is None:
                continue
            del self.pending[pid]
            self.send(BULK, pid, self.bulk_hdr(ep, OK, len(data)), data)

    def iso_thread(self):
        """isochronous IN packets and bulk IN data paced in real time"""
        period = self.dev.iso_period
        t = time.monotonic()
        n = 0
        while self.alive:
            t += 0.004
            now = time.monotonic()
            if t > now:
                time.sleep(t - now)
            else:
                t = now
            k = int(round(0.004 / period))
            for ep in list(self.iso_eps):
                for _ in range(k):
                    pkt = self.dev.iso_in(ep)
                    self.send(ISO, n, struct.pack("<BBH", ep, OK, len(pkt)), pkt)
                    n += 1
            for ep in list(self.int_eps):
                for _ in range(4):
                    pkt = self.dev.int_in(ep)
                    if pkt is None:
                        break
                    self.send(INTR, n, struct.pack("<BBH", ep, OK, len(pkt)), pkt)
                    n += 1
            if self.pending:
                self.serve_bulk()


# ---------------------------------------------------------------- devices
def desc(t, *b):
    body = bytes(b)
    return bytes([len(body) + 2, t]) + body


def w16(v):
    return [v & 0xFF, v >> 8]


def w32(v):
    return [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF]


class Device:
    speed = 1               # full speed
    mps0 = 64
    vid, pid = 0x1234, 0x0001
    iso_period = 0.001
    strings = {}

    def __init__(self):
        self.config = 0
        self.alts = {}
        self.redir = None

    def reset(self):
        self.config = 0
        self.alts = {}

    def string(self, i):
        if i == 0:
            return bytes([4, 3, 0x09, 0x04])
        s = self.strings.get(i)
        if s is None:
            return None
        e = s.encode("utf-16-le")
        return bytes([len(e) + 2, 3]) + e

    def control(self, rt, req, val, idx, ln, data):
        if rt == 0x80 and req == 6:
            t, i = val >> 8, val & 0xFF
            if t == 1:
                return OK, self.device_desc()
            if t == 2:
                return OK, self.config_desc()
            if t == 3:
                s = self.string(i)
                return (OK, s) if s else (STALL, b"")
            return STALL, b""
        if rt == 0x00 and req == 9:
            self.config = val
            return OK, b""
        if rt == 0x01 and req == 11:
            self.set_alt(idx, val)
            return OK, b""
        if rt == 0x80 and req == 0:
            return OK, b"\0\0"
        return self.class_control(rt, req, val, idx, ln, data)

    def class_control(self, rt, req, val, idx, ln, data):
        return STALL, b""

    def set_alt(self, iface, alt):
        self.alts[iface] = alt

    def bulk_out(self, ep, data):
        pass

    def bulk_in(self, ep, ln):
        return None

    def iso_in(self, ep):
        return b""

    def int_in(self, ep):
        return None


# ---- USB video class camera ----
NV12 = bytes([0x4E, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
YUY2 = bytes([0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])


def strip_dht(jpg):
    """MJPEG as cameras send it: no Huffman tables (the standard ones are implied)"""
    out = bytearray(jpg[:2])
    i = 2
    while i < len(jpg):
        if jpg[i] != 0xFF:
            break
        m = jpg[i + 1]
        if m == 0xDA:
            out += jpg[i:]
            break
        ln = (jpg[i + 2] << 8) | jpg[i + 3]
        if m != 0xC4:
            out += jpg[i:i + 2 + ln]
        i += 2 + ln
    return bytes(out)


class UVC(Device):
    vid, pid = 0x0C45, 0x6366
    strings = {1: "WINDRV Test", 2: "Test Camera"}
    # (format index, kind, [(w, h, fps)])
    formats = [(1, "yuy2", [(160, 120, 15), (320, 240, 5)]),
               (2, "mjpg", [(320, 240, 15), (640, 480, 10)])]
    bulk = False

    def __init__(self):
        super().__init__()
        self.probe = None
        self.frame_no = 0
        self.cur = None            # current frame bytes
        self.cur_off = 0
        self.fid = 0
        self.mps = 1023 if self.speed == 1 else 3072
        self.iso_period = 0.001 if self.speed == 1 else 0.000125
        self.last_frame_t = 0

    def device_desc(self):
        return desc(1, *w16(0x0200), 0xEF, 0x02, 0x01, self.mps0, *w16(self.vid), *w16(self.pid),
                    *w16(0x0100), 1, 2, 0, 1)

    def vs_descs(self):
        out = b""
        nfmt = len(self.formats)
        for fi, kind, frames in self.formats:
            if kind == "yuy2":
                out += desc(0x24, 0x04, fi, len(frames), *YUY2, 16, 1, 0, 0, 0, 0)
            elif kind == "nv12":
                out += desc(0x24, 0x04, fi, len(frames), *NV12, 12, 1, 0, 0, 0, 0)
            else:
                out += desc(0x24, 0x06, fi, len(frames), 1, 1, 0, 0, 0, 0)
            for k, (w, h, fps) in enumerate(frames, 1):
                iv = 10000000 // fps
                size = w * h * 3 // 2 if kind == "nv12" else w * h * 2
                out += desc(0x24, 0x07 if kind == "mjpg" else 0x05, k, 0, *w16(w), *w16(h),
                            *w32(size * 8 * fps), *w32(size * 8 * fps), *w32(size), *w32(iv), 1, *w32(iv))
            out += desc(0x24, 0x0D, 1, 1, 4)        # color matching
        ep = 0x81
        hdr = desc(0x24, 0x01, nfmt, *w16(0), ep, 0, 3, 0, 0, 0, 1, *([0] * nfmt))
        total = len(hdr) + len(out)
        hdr = desc(0x24, 0x01, nfmt, *w16(total), ep, 0, 3, 0, 0, 0, 1, *([0] * nfmt))
        return hdr + out

    def config_desc(self):
        vc = (desc(0x24, 0x01, *w16(0x0110), *w16(0), *w32(48000000), 1, 1) +
              desc(0x24, 0x02, 1, *w16(0x0201), 0, 0, *w16(0), *w16(0), *w16(0), 3, 0, 0, 0) +
              desc(0x24, 0x05, 2, 1, *w16(0), 2, 0x01, 0x00, 0) +
              desc(0x24, 0x03, 3, *w16(0x0101), 0, 2, 0))
        vc = desc(0x24, 0x01, *w16(0x0110), *w16(len(vc)), *w32(48000000), 1, 1) + vc[13:]
        body = desc(11, 0, 2, 0x0E, 0x03, 0x00, 2)                     # IAD
        body += desc(4, 0, 0, 0, 0x0E, 0x01, 0x00, 2) + vc              # VideoControl
        vs = self.vs_descs()
        if self.bulk:
            body += desc(4, 1, 0, 1, 0x0E, 0x02, 0x00, 0) + vs
            body += desc(5, 0x81, 0x02, *w16(64 if self.speed == 1 else 512), 0)
        else:
            body += desc(4, 1, 0, 0, 0x0E, 0x02, 0x00, 0) + vs
            for alt, m in ((1, 256), (2, 512), (3, self.mps)):
                body += desc(4, 1, alt, 1, 0x0E, 0x02, 0x00, 0)
                # high bandwidth: bits 12..11 = additional transactions per microframe
                enc = m if m <= 1024 else ((m + 1023) // 1024 - 1) << 11 | 1024
                body += desc(5, 0x81, 0x05, *w16(enc), 1)
        cfg = desc(2, *w16(0), 2, 1, 0, 0x80, 250)
        total = len(cfg) + len(body)
        return desc(2, *w16(total), 2, 1, 0, 0x80, 250) + body

    def interfaces(self):
        return [(0, 0x0E, 1, 0), (1, 0x0E, 2, 0)]

    def endpoints(self):
        if self.bulk:
            return [(0x81, 2, 0, 1, 64 if self.speed == 1 else 512)]
        alt = self.alts.get(1, 0)
        if alt == 0:
            return []
        m = {1: 256, 2: 512, 3: self.mps}[alt]
        return [(0x81, 1, 1, 1, m if m <= 1024 else ((m + 1023) // 1024 - 1) << 11 | 1024)]

    def mode(self):
        p = self.probe or bytes(34)
        fi, fr = p[2] or 1, p[3] or 1
        for f, kind, frames in self.formats:
            if f == fi:
                w, h, fps = frames[min(fr, len(frames)) - 1]
                return kind, w, h, fps
        return "yuy2", 160, 120, 15

    def probe_reply(self, data):
        p = bytearray(data.ljust(34, b"\0"))
        if p[2] == 0:
            p[2] = 1
        if p[3] == 0:
            p[3] = 1
        kind, w, h, fps = self.mode_for(p[2], p[3])
        struct.pack_into("<I", p, 4, 10000000 // fps)
        struct.pack_into("<I", p, 18, w * h * 3 // 2 if kind == "nv12" else w * h * 2)
        # bulk: one payload per frame (as many bulk cameras); iso: one payload per packet
        struct.pack_into("<I", p, 22, w * h * 2 + 2 if self.bulk else self.mps)
        return bytes(p)

    def mode_for(self, fi, fr):
        for f, kind, frames in self.formats:
            if f == fi:
                w, h, fps = frames[min(max(fr, 1), len(frames)) - 1]
                return kind, w, h, fps
        return "yuy2", 160, 120, 15

    def class_control(self, rt, req, val, idx, ln, data):
        cs = val >> 8
        if (idx & 0xFF) == 1 and cs in (1, 2):                  # VS probe / commit
            if rt == 0x21 and req == 0x01:
                self.probe = self.probe_reply(data)
                if cs == 2:
                    log("commit: format %d frame %d" % (self.probe[2], self.probe[3]), self.mode())
                    self.cur = None
                return OK, b""
            if rt == 0xA1:
                if req == 0x85:
                    return OK, bytes([34, 0])
                if req == 0x86:
                    return OK, bytes([3])
                return OK, (self.probe or self.probe_reply(b""))[:ln]
        return STALL, b""

    def make_frame(self):
        kind, w, h, fps = self.mode()
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(im)
        bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
        for i, c in enumerate(bars):
            d.rectangle([i * w // 8, 0, (i + 1) * w // 8 - 1, h - 1], fill=c)
        x = (self.frame_no * 4) % (w - h // 4)
        d.rectangle([x, h // 2, x + h // 4, h // 2 + h // 4], fill=(128, 64, 32))
        d.text((4, 4), "WINDRV %d" % self.frame_no, fill=(0, 0, 0))
        self.frame_no += 1
        if kind == "mjpg":
            b = io.BytesIO()
            im.save(b, "JPEG", quality=75, subsampling=1)
            return strip_dht(b.getvalue())
        import numpy as np
        if kind == "nv12":
            a = np.asarray(im).astype(np.int32)
            r, g, bl = a[..., 0], a[..., 1], a[..., 2]
            y = (((66 * r + 129 * g + 25 * bl + 128) >> 8) + 16).clip(0, 255).astype(np.uint8)
            u = ((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128
            v = ((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128
            u2 = ((u[0::2, 0::2] + u[1::2, 0::2] + u[0::2, 1::2] + u[1::2, 1::2]) // 4).clip(0, 255)
            v2 = ((v[0::2, 0::2] + v[1::2, 0::2] + v[0::2, 1::2] + v[1::2, 1::2]) // 4).clip(0, 255)
            uv = np.zeros((h // 2, w), dtype=np.uint8)
            uv[:, 0::2] = u2
            uv[:, 1::2] = v2
            return y.tobytes() + uv.tobytes()
        # YUY2
        a = np.asarray(im).astype(np.int32)
        r, g, bl = a[..., 0], a[..., 1], a[..., 2]
        y = ((66 * r + 129 * g + 25 * bl + 128) >> 8) + 16
        u = ((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128
        v = ((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128
        out = np.zeros((h, w * 2), dtype=np.uint8)
        out[:, 0::2] = y.clip(0, 255)
        out[:, 1::4] = ((u[:, 0::2] + u[:, 1::2]) // 2).clip(0, 255)
        out[:, 3::4] = ((v[:, 0::2] + v[:, 1::2]) // 2).clip(0, 255)
        return out.tobytes()

    def next_payload(self, room):
        """one payload: 2-byte header + frame data"""
        kind, w, h, fps = self.mode()
        now = time.monotonic()
        if self.cur is None:
            if now - self.last_frame_t < 1.0 / fps:
                return bytes([2, 0x80 | self.fid])
            self.last_frame_t = now
            self.cur = self.make_frame()
            self.cur_off = 0
        n = min(room - 2, len(self.cur) - self.cur_off)
        chunk = self.cur[self.cur_off:self.cur_off + n]
        self.cur_off += n
        eof = self.cur_off >= len(self.cur)
        hdr = bytes([2, 0x80 | self.fid | (2 if eof else 0)])
        if eof:
            self.cur = None
            self.fid ^= 1
        return hdr + chunk

    def iso_in(self, ep):
        alt = self.alts.get(1, 0)
        if not alt or not self.probe:
            return b""
        return self.next_payload({1: 256, 2: 512, 3: self.mps}[alt])

    def bulk_in(self, ep, ln):
        if not self.probe or ln < 3:
            return None
        if self.cur is None:
            kind, w, h, fps = self.mode()
            if time.monotonic() - self.last_frame_t < 1.0 / fps:
                return None
        # bulk: one payload per frame, transfers of up to ln bytes
        if self.cur is None:
            self.last_frame_t = time.monotonic()
            self.cur = bytes([2, 0x80 | self.fid | 2]) + self.make_frame()
            self.cur_off = 0
            self.fid ^= 1
        if self.cur_off >= len(self.cur):
            self.cur = None            # payload ended on a transfer boundary: zero length packet
            return b""
        chunk = self.cur[self.cur_off:self.cur_off + ln]
        self.cur_off += len(chunk)
        if self.cur_off >= len(self.cur) and len(chunk) < ln:
            self.cur = None
        return chunk


class UVCBulk(UVC):
    bulk = True


class UVCHS(UVC):
    speed = 2


# ---- HID precision touchpad ----
def ptp_finger():
    return bytes([0x09, 0x22, 0xA1, 0x02,
                  0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x09, 0x47, 0x81, 0x02,   # confidence
                  0x09, 0x42, 0x81, 0x02,                                                 # tip
                  0x95, 0x06, 0x81, 0x03,
                  0x75, 0x08, 0x25, 0x0F, 0x95, 0x01, 0x09, 0x51, 0x81, 0x02,             # contact ID
                  0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x11,
                  0x09, 0x30, 0x35, 0x00, 0x46, 0x1A, 0x04, 0x81, 0x02,                   # X 0..4095 = 105 mm
                  0x26, 0xFF, 0x07, 0x46, 0x8A, 0x02, 0x09, 0x31, 0x81, 0x02,             # Y 0..2047 = 65 mm
                  0x05, 0x0D, 0xC0])


def ptp_rd(fingers):
    return (bytes([0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01]) + ptp_finger() * fingers + PTP_TAIL)


PTP_TAIL = (bytes([
    0x55, 0x0C, 0x66, 0x01, 0x10, 0x47, 0xFF, 0xFF, 0x00, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x01, 0x05, 0x0D, 0x09, 0x56, 0x81, 0x02,                         # scan time
    0x09, 0x54, 0x25, 0x7F, 0x95, 0x01, 0x75, 0x08, 0x81, 0x02,                         # contact count
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,             # button
    0x95, 0x07, 0x81, 0x03,
    0x05, 0x0D, 0x85, 0x02, 0x09, 0x55, 0x09, 0x59, 0x75, 0x04, 0x95, 0x02, 0x25, 0x0F, 0xB1, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xA1, 0x02,             # configuration
    0x09, 0x52, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0,       # input mode
    0x09, 0x22, 0xA1, 0x00, 0x85, 0x04, 0x09, 0x57, 0x09, 0x58, 0x75, 0x01, 0x95, 0x02,
    0x25, 0x01, 0xB1, 0x02, 0x95, 0x06, 0xB1, 0x03, 0xC0,                               # selective reporting
    0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x05, 0x09, 0x01, 0xA1, 0x00,             # mouse (mouse mode)
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0]))
PTP_RD = ptp_rd(3)

XU = 4095 / 105.0       # units per mm
YU = 2047 / 65.0


class PTP(Device):
    """USB precision touchpad that plays a fixed set of gestures once it is in touchpad mode:
       move 30 mm right, tap, two fingers 20 mm down, 10 mm up, two-finger tap, pad click"""
    vid, pid = 0x06CB, 0x7E7E
    strings = {1: "WINDRV Test", 2: "Test Touchpad"}
    slots = 3

    def __init__(self):
        super().__init__()
        self.mode = 0
        self.script = None
        self.t0 = 0
        self.scan = 0

    def device_desc(self):
        return desc(1, *w16(0x0200), 0, 0, 0, self.mps0, *w16(self.vid), *w16(self.pid), *w16(0x0100), 1, 2, 0, 1)

    def config_desc(self):
        body = desc(4, 0, 0, 1, 3, 0, 0, 0) + desc(0x21, *w16(0x0111), 0, 1, 0x22, *w16(len(ptp_rd(self.slots))))
        body += desc(5, 0x81, 0x03, *w16(64), 1)
        return desc(2, *w16(9 + len(body)), 1, 1, 0, 0x80, 50) + body

    def interfaces(self):
        return [(0, 3, 0, 0)]

    def endpoints(self):
        return [(0x81, 3, 1, 0, 64)]

    def class_control(self, rt, req, val, idx, ln, data):
        if rt == 0x81 and req == 6 and val >> 8 == 0x22:
            return OK, ptp_rd(self.slots)
        if rt == 0x21 and req == 9:
            log("SET_REPORT %04x" % val, data.hex())
            if val == 0x0303 and len(data) >= 2:
                self.mode = data[1]
                if self.mode == 3:
                    self.script = self.make_script()
                    self.t0 = time.monotonic() + 2.0
            return OK, b""
        if rt == 0x21 and req == 10:
            return OK, b""
        if rt == 0xA1 and req == 1:
            if val & 0xFF == 2:
                return OK, bytes([2, 0x03 | (0 << 4)])
            return OK, bytes([val & 0xFF] + [0] * 2)
        return STALL, b""

    def make_script(self):
        """list of (time, contacts [(id, x_mm, y_mm)], button) reports, 10 ms apart"""
        out, t = [], 0.0
        def add(cs, b=0):
            nonlocal t
            out.append((t, cs, b))
            t += 0.01
        def pause(s):
            nonlocal t
            t += s
        for i in range(51):                         # one finger, 30 mm right
            add([(1, 20 + 30 * i / 50, 30)])
        add([])
        pause(1.0)
        for i in range(8):                          # tap
            add([(2, 50, 30)])
        add([])
        pause(1.0)
        for i in range(51):                         # two fingers 20 mm down
            add([(3, 40, 20 + 20 * i / 50), (4, 60, 20 + 20 * i / 50)])
        add([])
        pause(1.0)
        for i in range(51):                         # two fingers 10 mm up
            add([(5, 40, 40 - 10 * i / 50), (6, 60, 40 - 10 * i / 50)])
        add([])
        pause(1.0)
        for i in range(8):                          # two-finger tap
            add([(7, 40, 30), (8, 60, 30)])
        add([])
        pause(1.0)
        for i in range(30):                         # pad click
            add([(9, 50, 50)], 1 if 5 <= i < 20 else 0)
        add([])
        return out

    def report(self, cs, b, count=None, step=True):
        if step:
            self.scan = (self.scan + 100) & 0xFFFF
        r = bytearray([1])
        for k in range(self.slots):
            if k < len(cs):
                cid, x, y = cs[k]
                r += bytes([0x03, cid & 15]) + struct.pack("<HH", int(x * XU), int(y * YU))
            else:
                r += bytes(6)
        r += struct.pack("<HBB", self.scan, len(cs) if count is None else count, b)
        return bytes(r)

    def int_in(self, ep):
        if getattr(self, "queue", None):
            return self.queue.pop(0)
        if self.mode != 3 or not self.script:
            return None
        now = time.monotonic() - self.t0
        if now < self.script[0][0]:
            return None
        t, cs, b = self.script.pop(0)
        if not cs:
            # all fingers lifted: last positions with the tip off
            return bytes([1] + [0] * (6 * self.slots)) + struct.pack("<HBB", self.scan, 1, 0)
        if len(cs) > self.slots:
            # hybrid reporting: the first report has the contact count, the others 0
            reps = []
            for k in range(0, len(cs), self.slots):
                reps.append(self.report(cs[k:k + self.slots], b, len(cs) if k == 0 else 0, k == 0))
            self.queue = reps[1:]
            return reps[0]
        return self.report(cs, b)


class PTP1(PTP):
    """the same touchpad with one contact per report (hybrid reporting)"""
    slots = 1


class UVCFull(UVC):
    """high speed camera with many picture sizes in all three formats"""
    speed = 2
    formats = [(1, "yuy2", [(160, 120, 30), (176, 144, 30), (320, 240, 15), (352, 288, 15), (640, 480, 5)]),
               (2, "mjpg", [(320, 240, 30), (640, 480, 15), (800, 600, 10), (1280, 720, 10)]),
               (3, "nv12", [(320, 240, 15), (640, 480, 5)])]


DEVICES = {"uvc": UVC, "uvcbulk": UVCBulk, "uvchs": UVCHS, "uvcfull": UVCFull, "ptp": PTP, "ptp1": PTP1}


def main():
    port = int(sys.argv[1])
    cls = DEVICES[sys.argv[2]]
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(1)
    log("listening on", port, "as", sys.argv[2])
    while True:
        s, _ = ls.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log("QEMU connected")
        Redir(s, cls()).run()


if __name__ == "__main__":
    main()
