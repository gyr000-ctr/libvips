/* Generate deterministic JPEG XL test assets for the jxlload animation tests.
 *
 * Build (requires libjxl >= 0.7 with the encoder and thread runner):
 *
 * 	cc -o make-test-jxl make-test-jxl.c \
 * 		$(pkg-config --cflags --libs libjxl libjxl_threads)
 *
 * Run:
 *
 * 	./make-test-jxl OUTDIR
 *
 * The encoder output is deterministic, so regenerating produces byte-identical
 * files. The generated *.jxl binaries are committed alongside this source so
 * the test-suite does not need libjxl's encoder at test time.
 *
 * Valid animation / multipage assets:
 *
 * 	anim-crop.jxl    4 RGBA uchar frames on a 32x24 canvas; the middle two
 * 	                 frames are cropped, offset and blended layers, so the
 * 	                 decoder must coalesce them onto the running canvas
 * 	anim-16bit.jxl   3 RGBA ushort frames with per-frame alpha and
 * 	                 distinct delays (50, 75, 100 ms)
 * 	multipage.jxl    4 pages, every frame duration 0xffffffff: a multipage
 * 	                 document rather than an animation (no delay/loop meta)
 * 	anim-loop.jxl    2 frames with num_loops = 3 and delays 200/500 ms
 * 	anim-meta.jxl    2 frames carrying Exif and XMP metadata boxes
 * 	anim-many.jxl    64 small frames (frame-count / streaming coverage)
 *
 * Corrupt-input assets:
 *
 * 	good-exif.jxl        well-formed Exif box (control case)
 * 	bad-exif-offset.jxl  Exif box whose 4-byte TIFF offset is past the box
 * 	bad-exif-short.jxl   Exif box payload shorter than 4 bytes
 * 	bad-exif-empty.jxl   zero-length Exif box
 * 	truncated-12/20/40.jxl  valid container signature cut mid codestream
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

typedef struct {
	uint8_t *data;
	size_t size;
	size_t cap;
} Buffer;

static int
write_file(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	int rc = fwrite(data, 1, size, f) == size ? 0 : -1;
	fclose(f);
	return rc;
}

/* Run the encoder to completion into a malloc'd, grown-on-demand buffer.
 */
static int
drain(JxlEncoder *enc, Buffer *out)
{
	out->cap = 4096;
	out->data = malloc(out->cap);
	out->size = 0;

	for (;;) {
		uint8_t *next = out->data + out->size;
		size_t avail = out->cap - out->size;
		JxlEncoderStatus st =
			JxlEncoderProcessOutput(enc, &next, &avail);
		out->size = next - out->data;

		if (st == JXL_ENC_SUCCESS)
			return 0;
		if (st != JXL_ENC_NEED_MORE_OUTPUT) {
			fprintf(stderr, "JxlEncoderProcessOutput failed\n");
			return -1;
		}
		out->cap *= 2;
		out->data = realloc(out->data, out->cap);
	}
}

static JxlEncoder *
new_encoder(void *runner)
{
	JxlEncoder *enc = JxlEncoderCreate(NULL);
	if (JxlEncoderSetParallelRunner(enc,
			JxlThreadParallelRunner, runner)) {
		fprintf(stderr, "JxlEncoderSetParallelRunner failed\n");
		return NULL;
	}
	/* Metadata boxes require the container format.
	 */
	JxlEncoderUseContainer(enc, JXL_TRUE);
	return enc;
}

static void
fill_rgba8(uint8_t *px, int w, int h, int frame, int cropped)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *p = px + (y * w + x) * 4;

			p[0] = (uint8_t) (40 * frame + x * 4);
			p[1] = (uint8_t) (60 + y * 3);
			p[2] = (uint8_t) (200 - 30 * frame);
			/* An alpha ramp across cropped layers makes blending
			 * observable in the coalesced output.
			 */
			p[3] = cropped ? (uint8_t) (80 + x * 6) : 255;
		}
}

static void
fill_rgba16(uint16_t *px, int w, int h, int frame)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint16_t *p = px + (y * w + x) * 4;

			p[0] = (uint16_t) (10000 * frame + x * 257);
			p[1] = 20000;
			p[2] = 40000;
			p[3] = (uint16_t) (30000 + x * 1000);
		}
}

static int
add_rgba8_frame(JxlEncoder *enc, int frame_index, uint32_t duration,
	int w, int h, int have_crop, int cx, int cy, int cw, int ch)
{
	JxlFrameHeader fh = { 0 };
	JxlEncoderFrameSettings *fs =
		JxlEncoderFrameSettingsCreate(enc, NULL);

	JxlEncoderSetFrameLossless(fs, JXL_TRUE);
	JxlEncoderFrameSettingsSetOption(fs,
		JXL_ENC_FRAME_SETTING_EFFORT, 3);

	fh.duration = duration;
	if (have_crop) {
		fh.layer_info.have_crop = JXL_TRUE;
		fh.layer_info.crop_x0 = cx;
		fh.layer_info.crop_y0 = cy;
		fh.layer_info.xsize = cw;
		fh.layer_info.ysize = ch;
		fh.layer_info.blend_info.blendmode = JXL_BLEND_BLEND;
	}
	if (JxlEncoderSetFrameHeader(fs, &fh)) {
		fprintf(stderr, "SetFrameHeader failed\n");
		return -1;
	}

	int fw = have_crop ? cw : w;
	int fhgt = have_crop ? ch : h;
	size_t npix = (size_t) fw * fhgt;
	uint8_t *px = malloc(npix * 4);
	fill_rgba8(px, fw, fhgt, frame_index, have_crop);

	JxlPixelFormat fmt = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	int rc = JxlEncoderAddImageFrame(fs, &fmt, px, npix * 4);
	free(px);
	if (rc)
		fprintf(stderr, "AddImageFrame failed\n");
	return rc ? -1 : 0;
}

static int
emit(const char *outdir, const char *name, const Buffer *out)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s", outdir, name);
	if (write_file(path, out->data, out->size)) {
		fprintf(stderr, "failed to write %s\n", path);
		return -1;
	}
	printf("generated %s (%zu bytes)\n", name, out->size);
	return 0;
}

/* anim-crop.jxl: 32x24 canvas, 4 frames; frames 2 and 3 are cropped layers.
 */
static int
gen_anim_crop(void *runner, const char *outdir)
{
	JxlEncoder *enc = new_encoder(runner);
	JxlBasicInfo info;

	JxlEncoderInitBasicInfo(&info);
	info.xsize = 32;
	info.ysize = 24;
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	info.num_extra_channels = 1;
	info.alpha_bits = 8;
	info.have_animation = JXL_TRUE;
	info.animation.tps_numerator = 100;
	info.animation.tps_denominator = 1;
	info.animation.num_loops = 0;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo(enc, &info))
		goto fail;

	if (add_rgba8_frame(enc, 0, 10, 32, 24, 0, 0, 0, 0, 0) ||
		add_rgba8_frame(enc, 1, 10, 32, 24, 1, 4, 3, 20, 16) ||
		add_rgba8_frame(enc, 2, 10, 32, 24, 1, 10, 8, 18, 14) ||
		add_rgba8_frame(enc, 3, 10, 32, 24, 0, 0, 0, 0, 0))
		goto fail;
	JxlEncoderCloseFrames(enc);

	Buffer out;
	if (drain(enc, &out))
		goto fail;
	int rc = emit(outdir, "anim-crop.jxl", &out);
	free(out.data);
	JxlEncoderDestroy(enc);
	return rc;

fail:
	JxlEncoderDestroy(enc);
	return -1;
}

/* anim-16bit.jxl: 3 RGBA ushort frames, delays 50/75/100 ms.
 */
static int
gen_anim_16bit(void *runner, const char *outdir)
{
	JxlEncoder *enc = new_encoder(runner);
	JxlBasicInfo info;

	JxlEncoderInitBasicInfo(&info);
	info.xsize = 16;
	info.ysize = 12;
	info.bits_per_sample = 16;
	info.num_color_channels = 3;
	info.num_extra_channels = 1;
	info.alpha_bits = 16;
	info.have_animation = JXL_TRUE;
	info.animation.tps_numerator = 1000;
	info.animation.tps_denominator = 1;
	info.animation.num_loops = 0;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo(enc, &info))
		goto fail;

	JxlPixelFormat fmt = { 4, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0 };
	for (int f = 0; f < 3; f++) {
		JxlEncoderFrameSettings *fs =
			JxlEncoderFrameSettingsCreate(enc, NULL);
		JxlEncoderSetFrameLossless(fs, JXL_TRUE);
		JxlFrameHeader fh = { 0 };
		fh.duration = 50 + f * 25;
		if (JxlEncoderSetFrameHeader(fs, &fh))
			goto fail;

		size_t npix = 16 * 12;
		uint16_t *px = malloc(npix * 4 * sizeof(uint16_t));
		fill_rgba16(px, 16, 12, f);
		int rc = JxlEncoderAddImageFrame(fs, &fmt, px,
			npix * 4 * sizeof(uint16_t));
		free(px);
		if (rc)
			goto fail;
	}
	JxlEncoderCloseFrames(enc);

	Buffer out;
	if (drain(enc, &out))
		goto fail;
	int rc = emit(outdir, "anim-16bit.jxl", &out);
	free(out.data);
	JxlEncoderDestroy(enc);
	return rc;

fail:
	JxlEncoderDestroy(enc);
	return -1;
}

/* Shared helper for the simple RGB uchar animations and multipage file.
 */
static int
gen_rgb_frames(void *runner, const char *outdir, const char *name,
	int xsize, int ysize, int n_frames, uint32_t tps_num,
	uint32_t num_loops, uint32_t duration)
{
	JxlEncoder *enc = new_encoder(runner);
	JxlBasicInfo info;

	JxlEncoderInitBasicInfo(&info);
	info.xsize = xsize;
	info.ysize = ysize;
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	info.have_animation = JXL_TRUE;
	info.animation.tps_numerator = tps_num;
	info.animation.tps_denominator = 1;
	info.animation.num_loops = num_loops;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo(enc, &info))
		goto fail;

	JxlPixelFormat fmt = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	size_t npix = (size_t) xsize * ysize;
	for (int f = 0; f < n_frames; f++) {
		JxlEncoderFrameSettings *fs =
			JxlEncoderFrameSettingsCreate(enc, NULL);
		JxlEncoderSetFrameLossless(fs, JXL_TRUE);
		JxlFrameHeader fh = { 0 };
		fh.duration = duration;
		if (JxlEncoderSetFrameHeader(fs, &fh))
			goto fail;

		uint8_t *px = malloc(npix * 3);
		for (size_t i = 0; i < npix; i++) {
			px[i * 3 + 0] = (uint8_t) (f * 60);
			px[i * 3 + 1] = (uint8_t) (100 + i);
			px[i * 3 + 2] = (uint8_t) (255 - f * 40);
		}
		int rc = JxlEncoderAddImageFrame(fs, &fmt, px, npix * 3);
		free(px);
		if (rc)
			goto fail;
	}
	JxlEncoderCloseFrames(enc);

	Buffer out;
	if (drain(enc, &out))
		goto fail;
	int rc = emit(outdir, name, &out);
	free(out.data);
	JxlEncoderDestroy(enc);
	return rc;

fail:
	JxlEncoderDestroy(enc);
	return -1;
}

/* anim-meta.jxl: 2 frames plus Exif and XMP boxes.
 */
static int
gen_anim_meta(void *runner, const char *outdir)
{
	JxlEncoder *enc = new_encoder(runner);
	JxlBasicInfo info;

	JxlEncoderInitBasicInfo(&info);
	info.xsize = 8;
	info.ysize = 8;
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	info.have_animation = JXL_TRUE;
	info.animation.tps_numerator = 1000;
	info.animation.tps_denominator = 1;
	info.animation.num_loops = 0;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo(enc, &info))
		goto fail;

	JxlPixelFormat fmt = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	size_t npix = 8 * 8;
	for (int f = 0; f < 2; f++) {
		JxlEncoderFrameSettings *fs =
			JxlEncoderFrameSettingsCreate(enc, NULL);
		JxlEncoderSetFrameLossless(fs, JXL_TRUE);
		JxlFrameHeader fh = { 0 };
		fh.duration = 100;
		if (JxlEncoderSetFrameHeader(fs, &fh))
			goto fail;
		uint8_t *px = malloc(npix * 3);
		memset(px, f ? 200 : 40, npix * 3);
		int rc = JxlEncoderAddImageFrame(fs, &fmt, px, npix * 3);
		free(px);
		if (rc)
			goto fail;
	}
	JxlEncoderCloseFrames(enc);

	/* Exif box: 4-byte big-endian offset (0) then a tiny TIFF header.
	 */
	uint8_t exif[] = {
		0x00, 0x00, 0x00, 0x00,
		'I', 'I', 0x2a, 0x00,
		0x08, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	const char *xmp =
		"<?xpacket begin=''?><x:xmpmeta "
		"xmlns:x='adobe:ns:meta/'><rdf:RDF/></x:xmpmeta>";
	JxlEncoderUseBoxes(enc);
	if (JxlEncoderAddBox(enc, "Exif", exif, sizeof(exif), JXL_TRUE) ||
		JxlEncoderAddBox(enc, "xml ", (const uint8_t *) xmp,
			strlen(xmp), JXL_TRUE))
		goto fail;
	JxlEncoderCloseBoxes(enc);

	Buffer out;
	if (drain(enc, &out))
		goto fail;
	int rc = emit(outdir, "anim-meta.jxl", &out);
	free(out.data);
	JxlEncoderDestroy(enc);
	return rc;

fail:
	JxlEncoderDestroy(enc);
	return -1;
}

/* One still RGB frame; used as the base for the metadata-corruption files.
 */
static int
gen_still_container(JxlEncoder *enc)
{
	JxlBasicInfo info;

	JxlEncoderInitBasicInfo(&info);
	info.xsize = 8;
	info.ysize = 8;
	info.bits_per_sample = 8;
	info.num_color_channels = 3;
	info.uses_original_profile = JXL_TRUE;
	if (JxlEncoderSetBasicInfo(enc, &info))
		return -1;

	JxlPixelFormat fmt = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	uint8_t px[8 * 8 * 3];
	memset(px, 128, sizeof(px));
	JxlEncoderFrameSettings *fs =
		JxlEncoderFrameSettingsCreate(enc, NULL);
	JxlEncoderSetFrameLossless(fs, JXL_TRUE);
	if (JxlEncoderAddImageFrame(fs, &fmt, px, sizeof(px)))
		return -1;
	JxlEncoderCloseFrames(enc);
	return 0;
}

static int
gen_exif_case(void *runner, const char *outdir, const char *name,
	const uint8_t *payload, size_t payload_size)
{
	JxlEncoder *enc = new_encoder(runner);
	if (gen_still_container(enc))
		goto fail;
	JxlEncoderUseBoxes(enc);
	/* Uncompressed boxes keep the corrupt bytes predictable on disk.
	 */
	if (JxlEncoderAddBox(enc, "Exif", payload, payload_size, JXL_FALSE))
		goto fail;
	JxlEncoderCloseBoxes(enc);

	Buffer out;
	if (drain(enc, &out))
		goto fail;
	int rc = emit(outdir, name, &out);
	free(out.data);
	JxlEncoderDestroy(enc);
	return rc;

fail:
	JxlEncoderDestroy(enc);
	return -1;
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
		return 2;
	}
	const char *outdir = argv[1];
	mkdir(outdir, 0755);	/* ignore EEXIST */

	void *runner = JxlThreadParallelRunnerCreate(NULL, 4);
	int failures = 0;

	failures += gen_anim_crop(runner, outdir) != 0;
	failures += gen_anim_16bit(runner, outdir) != 0;
	/* multipage: all durations 0xffffffff => not an animation */
	failures += gen_rgb_frames(runner, outdir, "multipage.jxl",
		12, 10, 4, 1000, 0, 0xffffffff) != 0;
	failures += gen_anim_meta(runner, outdir) != 0;
	failures += gen_rgb_frames(runner, outdir, "anim-many.jxl",
		8, 8, 64, 1000, 0, 10) != 0;

	/* anim-loop: tps 10, durations 2 and 5 ticks => 200 and 500 ms,
	 * num_loops = 3.
	 */
	{
		JxlEncoder *enc = new_encoder(runner);
		JxlBasicInfo info;

		JxlEncoderInitBasicInfo(&info);
		info.xsize = 8;
		info.ysize = 8;
		info.bits_per_sample = 8;
		info.num_color_channels = 3;
		info.have_animation = JXL_TRUE;
		info.animation.tps_numerator = 10;
		info.animation.tps_denominator = 1;
		info.animation.num_loops = 3;
		info.uses_original_profile = JXL_TRUE;
		if (!JxlEncoderSetBasicInfo(enc, &info)) {
			JxlPixelFormat fmt = { 3, JXL_TYPE_UINT8,
				JXL_NATIVE_ENDIAN, 0 };
			int ok = 1;
			for (int f = 0; f < 2 && ok; f++) {
				JxlEncoderFrameSettings *fs =
					JxlEncoderFrameSettingsCreate(enc, NULL);
				JxlEncoderSetFrameLossless(fs, JXL_TRUE);
				JxlFrameHeader fh = { 0 };
				fh.duration = f == 0 ? 2 : 5;
				if (JxlEncoderSetFrameHeader(fs, &fh))
					ok = 0;
				uint8_t px[8 * 8 * 3];
				memset(px, f ? 200 : 40, sizeof(px));
				if (JxlEncoderAddImageFrame(fs, &fmt, px,
						sizeof(px)))
					ok = 0;
			}
			if (ok) {
				JxlEncoderCloseFrames(enc);
				Buffer out;
				if (!drain(enc, &out)) {
					failures += emit(outdir,
						"anim-loop.jxl", &out) != 0;
					free(out.data);
				}
				else
					failures++;
			}
			else
				failures++;
		}
		else
			failures++;
		JxlEncoderDestroy(enc);
	}

	/* Exif corruption cases.
	 */
	uint8_t exif_good[] = {
		0x00, 0x00, 0x00, 0x00,
		'I', 'I', 0x2a, 0x00,
		0x08, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	uint8_t exif_bad_offset[] = {
		0xff, 0xff, 0xff, 0xff,
		'I', 'I', 0x2a, 0x00,
		0x08, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	uint8_t exif_short[] = { 0x00, 0x00, 0x01 };
	failures += gen_exif_case(runner, outdir, "good-exif.jxl",
		exif_good, sizeof(exif_good)) != 0;
	failures += gen_exif_case(runner, outdir, "bad-exif-offset.jxl",
		exif_bad_offset, sizeof(exif_bad_offset)) != 0;
	failures += gen_exif_case(runner, outdir, "bad-exif-short.jxl",
		exif_short, sizeof(exif_short)) != 0;
	failures += gen_exif_case(runner, outdir, "bad-exif-empty.jxl",
		(const uint8_t *) "", 0) != 0;

	/* Truncated codestreams: encode a good still container then cut it.
	 */
	{
		JxlEncoder *enc = new_encoder(runner);
		if (gen_still_container(enc))
			failures++;
		else {
			Buffer out;
			if (drain(enc, &out))
				failures++;
			else {
				const int cuts[] = { 12, 20, 40 };
				for (size_t i = 0;
					i < sizeof(cuts) / sizeof(cuts[0]); i++) {
					if ((size_t) cuts[i] >= out.size)
						continue;
					char name[64];
					snprintf(name, sizeof(name),
						"truncated-%d.jxl", cuts[i]);
					char path[1024];
					snprintf(path, sizeof(path),
						"%s/%s", outdir, name);
					if (write_file(path, out.data, cuts[i]))
						failures++;
					else
						printf("generated %s (%d bytes)\n",
							name, cuts[i]);
				}
				free(out.data);
			}
		}
		JxlEncoderDestroy(enc);
	}

	JxlThreadParallelRunnerDestroy(runner);
	if (failures) {
		fprintf(stderr, "%d asset(s) failed to generate\n", failures);
		return 1;
	}
	return 0;
}
