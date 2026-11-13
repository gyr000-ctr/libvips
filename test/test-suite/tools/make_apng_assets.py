#!/usr/bin/env python3
"""Generate interlaced (Adam7) APNG test assets for libvips.

This is a deliberately small, dependency-free APNG writer: it emits raw
zlib streams, the APNG control chunks (acTL/fcTL/fdAT) and optional Adam7
interlacing by hand. Keeping it independent of libpng/libvips makes the
rendered test pixels an independent reference.

The same frame data is written twice for every asset:

* an Adam7 interlaced APNG (the thing libvips must learn to read), and
* a plain, non-interlaced APNG twin, which the existing libvips APNG
  pipeline already reads correctly.

The test-suite compares libvips's decode of the two byte-for-byte.

Run with::

    python3 make_apng_assets.py [output_directory]

The default output directory is ../images next to this script. The module
also exposes :class:`APNGWriter` so tests can synthesise corrupt files.
"""

import os
import struct
import zlib

# Adam7 pass geometry, copied from the PNG 1.2 specification (table 7.1).
# Each entry is (x_start, x_step, y_start, y_step).
ADAM7_PASSES = (
    (0, 8, 0, 8),
    (4, 8, 0, 8),
    (0, 4, 4, 8),
    (2, 4, 0, 4),
    (0, 2, 2, 4),
    (1, 2, 0, 2),
    (0, 1, 1, 2),
)

DISPOSE_NONE = 0
DISPOSE_BACKGROUND = 1
DISPOSE_PREVIOUS = 2

BLEND_SOURCE = 0
BLEND_OVER = 1

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def u32(value):
    return struct.pack(">I", value)


def u16(value):
    return struct.pack(">H", value)


def chunk(kind, data):
    return u32(len(data)) + kind + data + u32(
        zlib.crc32(kind + data) & 0xFFFFFFFF)


class APNGWriter:
    """Build one APNG file in memory.

    The canvas is @width x @height. Frames are (pixels, x, y, w, h,
    dispose, blend, delay_ms) tuples; pixels is a flat byte string holding
    w*h*@pixel_bytes RGBA samples. Every written frame uses the same colour
    type (8-bit or 16-bit RGBA).
    """

    def __init__(self, width, height, bitdepth=8, loop=0, interlaced=False,
            hidden_frame=None):
        self.width = width
        self.height = height
        self.bitdepth = bitdepth
        self.pixel_bytes = 4 if bitdepth == 8 else 8
        self.loop = loop
        self.interlaced = interlaced
        # Optional hidden default image: full-canvas RGBA bytes.
        self.hidden_frame = hidden_frame
        self.frames = []

    # ---- frame description ------------------------------------------------

    def add_frame(self, pixels, x=0, y=0, w=None, h=None,
            dispose=DISPOSE_NONE, blend=BLEND_SOURCE, delay_ms=100):
        w = self.width if w is None else w
        h = self.height if h is None else h
        assert len(pixels) == w * h * self.pixel_bytes
        self.frames.append((pixels, x, y, w, h, dispose, blend, delay_ms))

    # ---- pixel encoding ---------------------------------------------------

    def _frame_row(self, pixels, w, row):
        start = row * w * self.pixel_bytes
        return pixels[start:start + w * self.pixel_bytes]

    def _scanlines(self, pixels, w, h):
        """Filter type 0 scanlines for a frame's own pixel array."""
        out = bytearray()
        for row in range(h):
            out.append(0)  # PNG filter type: none
            out += self._frame_row(pixels, w, row)
        return bytes(out)

    def _interlaced_data(self, pixels, w, h):
        """Adam7 sub-images, each with its own filter-0 scanlines."""
        out = bytearray()
        for x_start, x_step, y_start, y_step in ADAM7_PASSES:
            cols = range(x_start, w, x_step)
            rows = range(y_start, h, y_step)
            sub_w = len(list(cols))
            sub_h = len(list(rows))
            if sub_w == 0 or sub_h == 0:
                continue
            for sy, src_y in enumerate(rows):
                out.append(0)
                for src_x in range(x_start, w, x_step):
                    idx = (src_y * w + src_x) * self.pixel_bytes
                    out += pixels[idx:idx + self.pixel_bytes]
        return bytes(out)

    def _frame_raw(self, pixels, w, h):
        """Uncompressed filter-0 bytes (optionally Adam7) for one frame."""
        if self.interlaced:
            return self._interlaced_data(pixels, w, h)
        return self._scanlines(pixels, w, h)

    # ---- chunk assembly ---------------------------------------------------

    def _fcTL(self, sequence, index, frame):
        _index = index
        _pixels, x, y, w, h, dispose, blend, delay_ms = frame
        # Keep the test delays exact at a 1000th of a second.
        num = delay_ms
        den = 1000
        # An fcTL data segment is 26 bytes: a 4 byte sequence number followed
        # by the 22 byte frame control fields (this matches libpng's writer
        # and png_ensure_sequence_number() reader exactly).
        fields = u32(w) + u32(h) + u32(x) + u32(y) + \
            u16(num) + u16(den) + bytes((dispose, blend))
        assert len(fields) == 22
        return chunk(b"fcTL", u32(sequence) + fields)

    def bytes(self):
        out = bytearray(PNG_SIGNATURE)
        sequence = 0

        # IHDR: every frame (including the default image) is Adam7 when the
        # animation is interlaced.
        interlace = 1 if self.interlaced else 0
        ihdr = struct.pack(">IIBBBBB", self.width, self.height,
            self.bitdepth, 6, 0, 0, interlace)  # colour type 6 = RGBA
        out += chunk(b"IHDR", ihdr)

        # num_frames counts the animation frames. A hidden default image is
        # not displayed and is excluded; libvips adjusts the count itself
        # when it sees the hidden flag.
        default_is_hidden = self.hidden_frame is not None
        num_frames = len(self.frames)
        out += chunk(b"acTL", u32(num_frames) + u32(self.loop))

        # libpng reclaims (inflateReset) its zstream at every frame boundary,
        # so every IDAT/fdAT payload must be a self-contained zlib stream:
        # compress each frame's raw filtered bytes independently.
        if default_is_hidden:
            frame_pieces = [
                self._frame_raw(self.hidden_frame, self.width, self.height)]
            frame_pieces += [
                self._frame_raw(f[0], f[3], f[4]) for f in self.frames]
        else:
            frame_pieces = [
                self._frame_raw(f[0], f[3], f[4]) for f in self.frames]

        compressed_pieces = [zlib.compress(piece, 9)
            for piece in frame_pieces]

        # Sequence numbers count fcTL and fdAT chunks in stream order,
        # starting at zero. IDAT chunks are not counted.
        sequence = 0

        if default_is_hidden:
            # Default image with no leading fcTL: legal only when hidden. It
            # has no fcTL, so the first animation fcTL starts the virtual
            # sequence at zero.
            out += chunk(b"IDAT", compressed_pieces[0])
            sequence = 0
            piece_index = 1
            for frame in self.frames:
                # fcTL occupies virtual sequence number @sequence.
                out += self._fcTL(sequence, piece_index - 1, frame)
                sequence += 1
                # fdAT stores the next sequence number in its first 4 bytes.
                out += chunk(b"fdAT",
                    u32(sequence) + compressed_pieces[piece_index])
                sequence += 1
                piece_index += 1
        else:
            # First animation frame is carried by IDAT and needs a fcTL, which
            # takes virtual sequence number zero. IDAT data does not consume a
            # sequence number. Each later frame's fcTL consumes one virtual
            # number and its fdAT stores the following one:
            #   fcTL(0), IDAT, fcTL(1), fdAT(2), fcTL(3), fdAT(4), ...
            out += self._fcTL(0, 0, self.frames[0])
            out += chunk(b"IDAT", compressed_pieces[0])

            sequence = 1
            piece_index = 1
            for frame in self.frames[1:]:
                out += self._fcTL(sequence, piece_index, frame)
                sequence += 1
                out += chunk(b"fdAT",
                    u32(sequence) + compressed_pieces[piece_index])
                sequence += 1
                piece_index += 1

        out += chunk(b"IEND", b"")

        return bytes(out)


def solid(w, h, rgba, bitdepth=8):
    """A flat w*h RGBA frame."""
    if bitdepth == 8:
        pixel = bytes(rgba)
    else:
        pixel = b"".join(struct.pack(">H", v) for v in rgba)
    return pixel * (w * h)


def rect(w, h, sub_x, sub_y, sub_w, sub_h, rgba, background=(0, 0, 0, 0),
        bitdepth=8):
    """A full w*h frame whose sub-rectangle is painted rgba."""
    if bitdepth == 8:
        bg = bytes(background)
        fg = bytes(rgba)
        row = bg * sub_x + fg * sub_w + bg * (w - sub_x - sub_w)
        return row * h
    bg = b"".join(struct.pack(">H", v) for v in background)
    fg = b"".join(struct.pack(">H", v) for v in rgba)
    row = bg * sub_x + fg * sub_w + bg * (w - sub_x - sub_w)
    return row * h


def _combo_frames():
    """Frames exercising every dispose/blend combination on an 8x8 canvas.

    Frame layout (all frames are sub-rectangles so offsets are covered):
      0 full frame,        dispose NONE,       blend SOURCE
      1 4x4 rect at (2,2), dispose BACKGROUND, blend SOURCE
      2 4x4 rect at (2,2), dispose PREVIOUS,   blend OVER
      3 2x2 rect at (3,3), dispose NONE,       blend OVER
      4 6x2 rect at (1,5), dispose BACKGROUND, blend OVER
      5 2x6 rect at (5,1), dispose PREVIOUS,   blend SOURCE
    """
    w = h = 8
    return [
        (solid(w, h, (255, 0, 0, 255)),
            0, 0, w, h, DISPOSE_NONE, BLEND_SOURCE, 100),
        (solid(4, 4, (0, 255, 0, 255)),
            2, 2, 4, 4, DISPOSE_BACKGROUND, BLEND_SOURCE, 150),
        (solid(4, 4, (0, 0, 255, 128)),
            2, 2, 4, 4, DISPOSE_PREVIOUS, BLEND_OVER, 200),
        (solid(2, 2, (255, 255, 0, 128)),
            3, 3, 2, 2, DISPOSE_NONE, BLEND_OVER, 250),
        (solid(6, 2, (0, 255, 255, 128)),
            1, 5, 6, 2, DISPOSE_BACKGROUND, BLEND_OVER, 300),
        (solid(2, 6, (255, 0, 255, 255)),
            5, 1, 2, 6, DISPOSE_PREVIOUS, BLEND_SOURCE, 350),
    ]


def _build_combo(interlaced):
    writer = APNGWriter(8, 8, bitdepth=8, loop=3, interlaced=interlaced)
    for frame in _combo_frames():
        writer.add_frame(*frame)
    return writer.bytes()


def _build_16bit(interlaced):
    """16-bit RGBA: half-alpha red base, half-alpha green OVER sub-rect."""
    writer = APNGWriter(8, 8, bitdepth=16, loop=0, interlaced=interlaced)
    writer.add_frame(solid(8, 8, (65535, 0, 0, 32768), bitdepth=16),
        dispose=DISPOSE_NONE, blend=BLEND_SOURCE, delay_ms=100)
    writer.add_frame(solid(4, 4, (0, 65535, 0, 32768), bitdepth=16),
        x=2, y=2, w=4, h=4,
        dispose=DISPOSE_BACKGROUND, blend=BLEND_OVER, delay_ms=200)
    return writer.bytes()


def _build_hidden(interlaced):
    """A hidden default image (white) followed by two animation frames."""
    writer = APNGWriter(8, 8, bitdepth=8, loop=1, interlaced=interlaced,
        hidden_frame=solid(8, 8, (255, 255, 255, 255)))
    writer.add_frame(solid(8, 8, (255, 0, 0, 255)),
        dispose=DISPOSE_NONE, blend=BLEND_SOURCE, delay_ms=100)
    writer.add_frame(solid(4, 4, (0, 255, 0, 255)),
        x=2, y=2, w=4, h=4,
        dispose=DISPOSE_BACKGROUND, blend=BLEND_SOURCE, delay_ms=200)
    return writer.bytes()


ASSETS = (
    # (base name, builder)
    ("apng-interlaced", _build_combo),
    ("apng-interlaced-16bit", _build_16bit),
    ("apng-interlaced-hidden", _build_hidden),
)


def _iter_chunks(data):
    """Yield (offset, length, kind) for every chunk in a PNG byte string."""
    offset = 8
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        yield offset, length, kind
        offset += 12 + length


def corrupt_frame_geometry(data):
    """Make a later frame extend past the canvas, keeping the CRC valid.

    Raises ValueError if the expected fcTL chunks cannot be found.
    """
    out = bytearray(data)
    fcTL_offsets = [offset for offset, length, kind in _iter_chunks(data)
        if kind == b"fcTL"]
    if len(fcTL_offsets) < 2:
        raise ValueError("need at least two fcTL chunks")

    offset = fcTL_offsets[1]
    length = struct.unpack(">I", out[offset:offset + 4])[0]
    kind = bytes(out[offset + 4:offset + 8])
    # fcTL data: 4 byte sequence, then width at byte 4. Oversize the width.
    struct.pack_into(">I", out, offset + 8 + 4, 0x7fffffff)
    fields = bytes(out[offset + 8:offset + 8 + length])
    struct.pack_into(">I", out, offset + 8 + length,
        zlib.crc32(kind + fields) & 0xFFFFFFFF)
    return bytes(out)


def truncate_file(data):
    """Drop the tail so the final frame's compressed data is incomplete."""
    return data[:max(8, len(data) - 40)]


def corrupt_fctl_length(data):
    """Declare an fcTL data length that is not the required 26 bytes."""
    out = bytearray(data)
    for offset, _length, kind in _iter_chunks(data):
        if kind == b"fcTL":
            struct.pack_into(">I", out, offset, 20)
            return bytes(out)
    raise ValueError("no fcTL chunk found")


def main(output_dir):
    os.makedirs(output_dir, exist_ok=True)
    for name, builder in ASSETS:
        interlaced = builder(True)
        progressive = builder(False)
        with open(os.path.join(output_dir, name + ".png"), "wb") as f:
            f.write(interlaced)
        with open(os.path.join(output_dir, name + "-progressive.png"), "wb") as f:
            f.write(progressive)
        print("wrote %s.png and %s-progressive.png" % (name, name))


if __name__ == "__main__":
    import sys

    if len(sys.argv) > 1:
        target = sys.argv[1]
    else:
        target = os.path.join(os.path.dirname(__file__), os.pardir, "images")
    main(os.path.abspath(target))

