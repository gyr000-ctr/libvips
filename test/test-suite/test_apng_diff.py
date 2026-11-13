# vim: set fileencoding=utf-8 :
"""Tests for the APNG inter-frame diff encoder in pngsave.

The old writer emitted every frame as a full-canvas DISPOSE_NONE /
BLEND_SOURCE image, so animations with a static background cost roughly one
full image per frame. The encoder now diffs each frame against the previous
one and writes only the tight bounding box of the changed pixels, keeping a
1x1 SOURCE frame for frames that do not change at all.

These tests check, for RGBA8 and RGBA16, across every interesting frame
shape (first frame, identical frame, fully transparent frame, local change,
random noise):

  * libvips' own pngload reproduces the input animation pixel-for-pixel;
  * a fully independent, stdlib-only APNG decoder (helpers/apng_decode.py)
    reproduces it pixel-for-pixel as well;
  * delay, loop count, page height and frame count are preserved;
  * the diff encoding is no larger than the old full-canvas encoding and is
    substantially smaller for the static-background frame shapes it
    targets;
  * the streaming writer never holds the whole animation: its tracked peak
    allocation does not grow with the number of frames.
"""

import ctypes
import os
import random
import struct

import pytest

import pyvips

from helpers import have_apng, APNG_ANIM_FILE
from apng_decode import APNGDecoder


W, H = 32, 24
LOOP = 5
DELAYS = [40, 70, 90, 110, 130, 150, 170]


def _solid(width, height, rgba, fmt):
    # add the constant first (float arithmetic), then cast so the result is
    # exactly the requested integer format
    im = pyvips.Image.black(width, height, bands=4) + list(rgba)
    return im.cast(fmt)


def _frame(kind, fmt):
    """One frame of the synthetic animation, rendered straight to RGBA."""
    max_v = 255 if fmt == "uchar" else 65535
    base = _solid(W, H, (40, 80, 160, max_v), fmt)

    if kind == "first":
        return base

    if kind == "identical":
        return base

    if kind == "transparent":
        # every pixel fully transparent, RGB still carrying colour
        return _solid(W, H, (90, 120, 200, 0), fmt)

    if kind == "local_opaque":
        patch = _solid(6, 5, (255, 0, 0, max_v), fmt)
        return base.insert(patch, 18, 14)

    if kind == "local_semi":
        half = max_v // 2
        patch = _solid(4, 4, (0, 255, 0, half), fmt)
        return base.insert(patch, 3, 17)

    if kind == "noise":
        rng = random.Random(1234)
        if fmt == "uchar":
            data = bytes(rng.randrange(256)
                for _ in range(W * H * 4))
        else:
            data = b"".join(
                rng.randrange(65536).to_bytes(2, "big")
                for _ in range(W * H * 4))
        return pyvips.Image.new_from_memory(
            data, W, H, 4, fmt).copy()

    raise ValueError(kind)


# Every frame shape the encoder must handle, in an order that keeps each
# frame's expected region easy to reason about:
#   0 full-canvas base (first frame)
#   1 exact duplicate (no pixel changes)
#   2 strict sub-rectangle opaque change
#   3 strict sub-rectangle semi-transparent change
#   4 whole canvas replaced by fully transparent pixels
#   5 whole-canvas random noise
#   6 opaque patch over the noise (changes the whole canvas back)
KINDS = ["first", "identical", "local_opaque", "local_semi",
    "transparent", "noise", "local_opaque"]
N = len(KINDS)


def _set_anim_meta(im, delays):
    im.set_type(pyvips.GValue.gint_type, "page-height", H)
    im.set_type(pyvips.GValue.array_int_type, "delay", delays)
    im.set_type(pyvips.GValue.gint_type, "loop", LOOP)


def _animation(fmt, kinds=None, delays=None):
    kinds = kinds or KINDS
    frames = [_frame(k, fmt) for k in kinds]
    im = frames[0].pagejoin(frames[1:])
    _set_anim_meta(im, delays or DELAYS[:len(frames)])
    return im


def _fctl_chunks(data):
    out = []
    off = 8
    while off + 12 <= len(data):
        length = struct.unpack(">I", data[off:off + 4])[0]
        ctype = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + length]
        if ctype == b"fcTL":
            out.append({
                "seq": struct.unpack(">I", body[0:4])[0],
                "w": struct.unpack(">I", body[4:8])[0],
                "h": struct.unpack(">I", body[8:12])[0],
                "x": struct.unpack(">I", body[12:16])[0],
                "y": struct.unpack(">I", body[16:20])[0],
                "dispose": body[24],
                "blend": body[25],
            })
        off += 12 + length
        if ctype == b"IEND":
            break
    return out


def _save(im, no_delta=False):
    """pngsave to a buffer, optionally forcing the legacy full-frame path."""
    old = os.environ.get("VIPS_PNG_APNG_NODELTA")
    if no_delta:
        os.environ["VIPS_PNG_APNG_NODELTA"] = "1"
    else:
        os.environ.pop("VIPS_PNG_APNG_NODELTA", None)
    try:
        # keep 16-bit precision when the source is 16-bit
        bitdepth = 16 if im.format == "ushort" else 8
        return im.pngsave_buffer(bitdepth=bitdepth)
    finally:
        if old is None:
            os.environ.pop("VIPS_PNG_APNG_NODELTA", None)
        else:
            os.environ["VIPS_PNG_APNG_NODELTA"] = old


def _frame_bytes(im):
    return im.write_to_memory()


@pytest.mark.skipif(not have_apng(),
    reason="no APNG support, skipping test")
class TestAPNGDiff:
    @pytest.mark.parametrize("fmt", ["uchar", "ushort"])
    def test_roundtrip_pixel_exact(self, fmt):
        im = _animation(fmt)
        buf = _save(im)

        # 1. libvips' own decoder
        back = pyvips.Image.pngload_buffer(buf, n=-1)
        assert back.width == W
        assert back.get("n-pages") == N
        assert back.get("page-height") == H
        assert back.get("delay") == DELAYS
        assert back.get("loop") == LOOP
        assert back.format == fmt
        assert (im - back).abs().max() == 0

        # 2. independent stdlib decoder
        dec = APNGDecoder(buf)
        delays, frames = dec.render()
        assert delays == DELAYS
        assert dec.num_plays == LOOP
        assert len(frames) == N
        source = _frame_bytes(im)
        sample_bytes = 1 if fmt == "uchar" else 2
        frame_size = W * H * 4 * sample_bytes
        for i, rendered in enumerate(frames):
            expected = source[i * frame_size:(i + 1) * frame_size]
            assert rendered == expected

    @pytest.mark.parametrize("fmt", ["uchar", "ushort"])
    def test_frame_regions_and_ops(self, fmt):
        buf = _save(_animation(fmt))
        chunks = _fctl_chunks(buf)
        assert len(chunks) == N

        # first frame always covers the full canvas and replaces it
        first = chunks[0]
        assert (first["x"], first["y"], first["w"], first["h"]) == \
            (0, 0, W, H)
        assert first["blend"] == 0  # BLEND_OP_SOURCE
        assert first["dispose"] == 0  # DISPOSE_OP_NONE

        def box(i):
            c = chunks[i]
            return c["x"], c["y"], c["w"], c["h"]

        # every frame stays on the canvas and covers at least one pixel;
        # we only ever emit SOURCE / DISPOSE_NONE
        for c in chunks:
            assert c["w"] > 0 and c["h"] > 0
            assert c["x"] + c["w"] <= W
            assert c["y"] + c["h"] <= H
            assert c["blend"] == 0
            assert c["dispose"] == 0

        # identical frame: tiny no-op rectangle
        assert box(1) == (0, 0, 1, 1)

        # local opaque change: strict sub-rectangle enclosing the patch
        assert box(2) != (0, 0, W, H)
        x, y, w, h = box(2)
        assert x <= 18 and y <= 14 and x + w >= 18 + 6 and y + h >= 14 + 5

        # local semi-transparent change: same, around its own patch
        assert box(3) != (0, 0, W, H)
        x, y, w, h = box(3)
        assert x <= 3 and y <= 17 and x + w >= 3 + 4 and y + h >= 17 + 4

        # fully transparent frame and noise frame: the whole canvas changes
        assert box(4) == (0, 0, W, H)
        assert box(5) == (0, 0, W, H)

    def test_size_vs_full_canvas(self):
        # A static background with a small opaque patch moving around it:
        # exactly the workload the diff encoder targets. Built on a larger
        # canvas so the per-file container overhead is negligible.
        w, h = 128, 96
        base = _solid(w, h, (40, 80, 160, 255), "uchar")
        n_frames = 16
        frames = [base]
        for i in range(1, n_frames):
            px = 8 + (i * 23) % (w - 20)
            py = 8 + (i * 17) % (h - 16)
            patch = _solid(10, 8, (255, 0, 0, 255), "uchar")
            frames.append(base.insert(patch, px, py))
        im = frames[0].pagejoin(frames[1:])
        im.set_type(pyvips.GValue.gint_type, "page-height", h)
        im.set_type(pyvips.GValue.array_int_type, "delay",
            [50] * n_frames)
        im.set_type(pyvips.GValue.gint_type, "loop", LOOP)

        diff = _save(im, no_delta=False)
        full = _save(im, no_delta=True)

        # the legacy file really is one full-canvas fcTL per frame
        for c in _fctl_chunks(full):
            assert (c["x"], c["y"], c["w"], c["h"]) == (0, 0, w, h)

        # diff encoding can never be larger, and for this static-background
        # animation it must be far smaller than one image per frame
        assert len(diff) <= len(full)
        assert len(diff) / len(full) < 0.6

        # the same guarantee for 16-bit
        base16 = _solid(w, h, (40, 80, 160, 65535), "ushort")
        frames16 = [base16]
        for i in range(1, n_frames):
            px = 8 + (i * 23) % (w - 20)
            py = 8 + (i * 17) % (h - 16)
            patch = _solid(10, 8, (255, 0, 0, 65535), "ushort")
            frames16.append(base16.insert(patch, px, py))
        im16 = frames16[0].pagejoin(frames16[1:])
        im16.set_type(pyvips.GValue.gint_type, "page-height", h)
        im16.set_type(pyvips.GValue.array_int_type, "delay",
            [50] * n_frames)
        im16.set_type(pyvips.GValue.gint_type, "loop", LOOP)
        diff16 = _save(im16, no_delta=False)
        full16 = _save(im16, no_delta=True)
        assert len(diff16) <= len(full16)
        assert len(diff16) / len(full16) < 0.6

    def test_real_animation_shrinks_and_matches(self):
        im = pyvips.Image.new_from_file(APNG_ANIM_FILE, n=-1)
        diff = _save(im, no_delta=False)
        full = _save(im, no_delta=True)

        back = pyvips.Image.pngload_buffer(diff, n=-1)
        assert (im - back).abs().max() == 0

        dec = APNGDecoder(diff)
        _, frames = dec.render()
        source = _frame_bytes(im)
        w, page_h = im.width, im.get("page-height")
        frame_size = w * page_h * 4
        for i, rendered in enumerate(frames):
            assert rendered == source[i * frame_size:(i + 1) * frame_size]

        assert len(diff) < len(full)

    def test_peak_memory_independent_of_frame_count(self):
        # The encoder must stream frames and never accumulate them. We make
        # the source images fully resident first (copy_memory), so any
        # frame-count-dependent footprint is already present before the
        # writer runs. A writer that buffered the whole animation would
        # raise the tracked-memory highwater when saving a 40-frame image
        # compared with an 8-frame one; the streaming writer's extra peak
        # does not depend on frame count at all.
        libvips = ctypes.CDLL("libvips.so.42")
        libvips.vips_tracked_get_mem_highwater.restype = ctypes.c_size_t
        saved_concurrency = pyvips.concurrency_get()
        pyvips.concurrency_set(1)
        try:
            result = self._peak_memory_body(libvips)
        finally:
            pyvips.concurrency_set(saved_concurrency)
        return result

    def _peak_memory_body(self, libvips):
        w, h = 128, 96
        frame_size = w * h * 4

        def materialized(n):
            base = _solid(w, h, (40, 80, 160, 255), "uchar")
            frames = [base]
            for i in range(1, n):
                patch = _solid(8, 8, (255, 0, 0, 255), "uchar")
                frames.append(base.insert(
                    patch, i * 5 % (w - 12), i * 7 % (h - 12)))
            im = frames[0].pagejoin(frames[1:])
            im.set_type(pyvips.GValue.gint_type, "page-height", h)
            im.set_type(pyvips.GValue.array_int_type, "delay",
                [40] * n)
            im.set_type(pyvips.GValue.gint_type, "loop", 0)
            return im.copy_memory()

        # settle one-off allocations, then keep both sources resident so the
        # larger one cannot itself move the save-time highwater
        _ = materialized(2).pngsave_buffer()
        short_im = materialized(8)
        long_im = materialized(40)
        extra_frames = 40 - 8

        short_buf = short_im.pngsave_buffer()
        high_after_short = libvips.vips_tracked_get_mem_highwater()
        long_buf = long_im.pngsave_buffer()
        high_after_long = libvips.vips_tracked_get_mem_highwater()

        # 32 extra frames would cost this many raw bytes if the writer held
        # them; the streaming writer adds nothing frame-count-dependent
        raw_extra = extra_frames * frame_size
        assert high_after_long - high_after_short < raw_extra // 4

        # both outputs are pixel-exact
        assert (short_im -
            pyvips.Image.pngload_buffer(short_buf, n=-1)).abs().max() == 0
        assert (long_im -
            pyvips.Image.pngload_buffer(long_buf, n=-1)).abs().max() == 0
