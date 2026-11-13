"""Minimal, self-contained APNG decoder used as an independent test oracle.

Standard library only (struct, zlib): it parses the PNG/APNG container,
inflates the default image (IDAT) and every fdAT stream, reverses the PNG
row filters and applies the APNG dispose/blend rendering model, returning
each presented frame as a straight RGBA byte buffer (8- or 16-bit).

It is deliberately independent of libpng and libvips, so a file that both
this decoder and libvips render pixel-for-pixel gives cross-implementation
confidence in the encoder.

Non-interlaced, bit depth 8/16, colour types gray/RGB and their alpha
variants -- everything pngsave emits.
"""

import struct
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"

DISPOSE_NONE = 0
DISPOSE_BACKGROUND = 1
DISPOSE_PREVIOUS = 2

BLEND_SOURCE = 0
BLEND_OVER = 1


def _u32(b, o=0):
    return struct.unpack(">I", b[o:o + 4])[0]


def _u16(b, o=0):
    return struct.unpack(">H", b[o:o + 2])[0]


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _unfilter(raw, width, height, bpp):
    stride = width * bpp
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 1:  # sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xff
        elif ftype == 2:  # up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif ftype == 3:  # average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif ftype == 4:  # paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xff
        elif ftype != 0:
            raise ValueError("bad PNG filter %d" % ftype)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return bytes(out)


def _iter_chunks(data):
    if data[:8] != PNG_SIG:
        raise ValueError("not a PNG")
    off = 8
    while off + 12 <= len(data):
        length = _u32(data, off)
        ctype = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + length]
        yield ctype, body
        off += 12 + length
        if ctype == b"IEND":
            break


class APNGDecoder:
    def __init__(self, data):
        self.data = data
        self.width = self.height = self.bitdepth = self.color_type = 0
        self.frames = []
        self.num_plays = 0
        self.hidden = False
        self._idat = bytearray()
        self._streams = []
        self._current = None
        self._saw_idat = False
        self._parse()

    def _parse(self):
        for ctype, body in _iter_chunks(self.data):
            if ctype == b"IHDR":
                self.width = _u32(body, 0)
                self.height = _u32(body, 4)
                self.bitdepth = body[8]
                self.color_type = body[9]
            elif ctype == b"acTL":
                self.num_plays = _u32(body, 4)
            elif ctype == b"fcTL":
                seq = _u32(body, 0)
                if not self.frames and self._saw_idat:
                    self.hidden = True
                self.frames.append({
                    "seq": seq,
                    "w": _u32(body, 4),
                    "h": _u32(body, 8),
                    "x": _u32(body, 12),
                    "y": _u32(body, 16),
                    "delay_num": _u16(body, 20),
                    "delay_den": _u16(body, 22),
                    "dispose": body[24],
                    "blend": body[25],
                })
                if seq == 0 and not self.hidden:
                    # frame 0 reuses the IDAT stream
                    self._streams.append(None)
                else:
                    stream = bytearray()
                    self._streams.append(stream)
                    self._current = stream
            elif ctype == b"IDAT":
                self._saw_idat = True
                self._idat += body
            elif ctype == b"fdAT" and self._current is not None:
                self._current.extend(body[4:])

    def _bpp(self):
        channels = {0: 1, 2: 3, 4: 2, 6: 4}[self.color_type]
        sample_bytes = 1 if self.bitdepth <= 8 else 2
        return channels * sample_bytes, channels, sample_bytes

    def _decode(self, zstream, w, h):
        """Inflate, unfilter and expand one image to straight RGBA.

        16-bit samples are converted from PNG big-endian to native byte
        order so the buffer can be compared directly with a libvips USHORT
        image in memory.
        """
        raw = zlib.decompress(bytes(zstream))
        bpp, channels, sample_bytes = self._bpp()
        pixels = _unfilter(raw, w, h, bpp)
        out = bytearray(w * h * 4 * sample_bytes)
        for y in range(h):
            for x in range(w):
                base = (y * w + x) * channels * sample_bytes
                o = (y * w + x) * 4 * sample_bytes
                if sample_bytes == 2:
                    vals = [_u16(pixels, base + 2 * k)
                        for k in range(channels)]
                    if channels == 4:
                        r, g, b, a = vals
                    elif channels == 3:
                        r, g, b, a = vals + [65535]
                    elif channels == 2:
                        r = g = b = vals[0]
                        a = vals[1]
                    else:
                        r = g = b = vals[0]
                        a = 65535
                    struct.pack_into("=HHHH", out, o, r, g, b, a)
                else:
                    vals = list(pixels[base:base + channels])
                    if channels == 4:
                        r, g, b, a = vals
                    elif channels == 3:
                        r, g, b, a = vals + [255]
                    elif channels == 2:
                        r = g = b = vals[0]
                        a = vals[1]
                    else:
                        r = g = b = vals[0]
                        a = 255
                    out[o:o + 4] = bytes((r, g, b, a))
        return bytes(out)

    def _blend(self, canvas, frame, fctl, w, h, sample_bytes):
        comps = 4 * sample_bytes
        max_a = (1 << (8 * sample_bytes)) - 1
        for y in range(h):
            for x in range(w):
                s = (y * w + x) * comps
                d = ((fctl["y"] + y) * self.width +
                    (fctl["x"] + x)) * comps
                if fctl["blend"] == BLEND_SOURCE:
                    canvas[d:d + comps] = frame[s:s + comps]
                    continue
                # BLEND_OVER, unpremultiplied "over" with the same integer
                # rounding vips_region_blend_over_line*() uses.
                if sample_bytes == 1:
                    sa = frame[s + 3]
                    if sa == 0:
                        continue
                    da = canvas[d + 3]
                    if sa == 255 or da == 0:
                        canvas[d:d + 4] = frame[s:s + 4]
                        continue
                    bf = (da * (255 - sa) + 127) // 255
                    out_a = sa + bf
                    for k in range(3):
                        canvas[d + k] = (frame[s + k] * sa +
                            canvas[d + k] * bf + out_a // 2) // out_a
                    canvas[d + 3] = out_a
                else:
                    sa = struct.unpack("=H", frame[s + 6:s + 8])[0]
                    if sa == 0:
                        continue
                    da = struct.unpack("=H", canvas[d + 6:d + 8])[0]
                    if sa == max_a or da == 0:
                        canvas[d:d + 8] = frame[s:s + 8]
                        continue
                    half = max_a // 2
                    bf = (da * (max_a - sa) + half) // max_a
                    out_a = sa + bf
                    for k in range(3):
                        sv = struct.unpack("=H",
                            frame[s + 2 * k:s + 2 * k + 2])[0]
                        dv = struct.unpack("=H",
                            canvas[d + 2 * k:d + 2 * k + 2])[0]
                        struct.pack_into("=H", canvas, d + 2 * k,
                            (sv * sa + dv * bf + out_a // 2) // out_a)
                    struct.pack_into("=H", canvas, d + 6, out_a)

    def render(self):
        """Return (delays_ms, [RGBA frame buffers]) after compositing.

        8-bit samples are one byte each; 16-bit samples are native-endian
        unsigned 16-bit values, matching a libvips USHORT image written to
        memory.
        """
        if not self.frames:
            raise ValueError("not an APNG")
        sample_bytes = 1 if self.bitdepth <= 8 else 2
        comps = 4 * sample_bytes
        canvas = bytearray(self.width * self.height * comps)
        saved = bytearray(len(canvas))
        frames_out, delays = [], []

        if self.hidden:
            hidden = self._decode(self._idat, self.width, self.height)
            self._blend(canvas, hidden,
                {"x": 0, "y": 0, "blend": BLEND_SOURCE},
                self.width, self.height, sample_bytes)

        for i, fctl in enumerate(self.frames):
            if not self.hidden and i == 0:
                zstream = self._idat
            else:
                zstream = self._streams[i]
            frame = self._decode(zstream, fctl["w"], fctl["h"])

            if fctl["dispose"] == DISPOSE_PREVIOUS:
                self._copy(saved, canvas, fctl, sample_bytes)
            self._blend(canvas, frame, fctl,
                fctl["w"], fctl["h"], sample_bytes)

            frames_out.append(bytes(canvas))
            den = fctl["delay_den"] or 100
            delays.append(round(1000 * fctl["delay_num"] / den))

            if fctl["dispose"] == DISPOSE_BACKGROUND:
                self._clear(canvas, fctl, sample_bytes)
            elif fctl["dispose"] == DISPOSE_PREVIOUS:
                self._restore(canvas, saved, fctl, sample_bytes)

        return delays, frames_out

    def _clear(self, canvas, fctl, sample_bytes):
        comps = 4 * sample_bytes
        row = b"\x00" * (fctl["w"] * comps)
        for y in range(fctl["y"], fctl["y"] + fctl["h"]):
            off = (y * self.width + fctl["x"]) * comps
            canvas[off:off + fctl["w"] * comps] = row

    def _copy(self, saved, canvas, fctl, sample_bytes):
        comps = 4 * sample_bytes
        for y in range(fctl["y"], fctl["y"] + fctl["h"]):
            off = (y * self.width + fctl["x"]) * comps
            n = fctl["w"] * comps
            saved[off:off + n] = canvas[off:off + n]

    def _restore(self, canvas, saved, fctl, sample_bytes):
        comps = 4 * sample_bytes
        for y in range(fctl["y"], fctl["y"] + fctl["h"]):
            off = (y * self.width + fctl["x"]) * comps
            n = fctl["w"] * comps
            canvas[off:off + n] = saved[off:off + n]
