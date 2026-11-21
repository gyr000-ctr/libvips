/* Test .cube 3D LUT loading, trilinear interpolation, 1D shaper handling,
 * error reporting and the cubelut operation.
 *
 * Run as part of: meson test -C build 3dlut
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/colour.h>

static int failures;
static int checks;

#define CHECK(cond) \
	G_STMT_START \
	{ \
		checks++; \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, \
				__LINE__, #cond); \
			failures++; \
		} \
	} \
	G_STMT_END

static void
check_error(GError *error, VipsCubeLUTError code, const char *label)
{
	checks++;
	if (!error) {
		fprintf(stderr, "FAIL %s: expected error, got none\n", label);
		failures++;
	}
	else if (error->code != code) {
		fprintf(stderr,
			"FAIL %s: expected error code %d, got %d (%s)\n",
			label, code, error->code, error->message);
		failures++;
	}
	else
		printf("ok %s: %s\n", label, error->message);
}

/* Write text to a fresh file in the test working directory.
 */
static char *
write_file(const char *name, const char *text)
{
	char *path = g_build_filename(".", name, NULL);
	GError *error = NULL;

	if (!g_file_set_contents(path, text, -1, &error))
		vips_error_exit("writing %s failed: %s", name,
			error->message);

	return path;
}

static gboolean
float_near(float a, float b)
{
	return fabsf(a - b) <= 1e-5f * (1.0f + fabsf(b));
}

static void
check_rgb(const char *label, const float got[3],
	float r, float g, float b)
{
	CHECK(float_near(got[0], r));
	CHECK(float_near(got[1], g));
	CHECK(float_near(got[2], b));
	if (!float_near(got[0], r) ||
		!float_near(got[1], g) ||
		!float_near(got[2], b))
		fprintf(stderr, "  %s: got (%g %g %g), expected (%g %g %g)\n",
			label, got[0], got[1], got[2], r, g, b);
}

/* Fetch one pixel as doubles.
 */
static double *
get_pixel(VipsImage *image)
{
	double *vector;
	int n;

	if (vips_getpoint(image, &vector, &n, 0, 0, NULL))
		vips_error_exit(NULL);

	return vector;
}

/* Build a 2x2x2 cube whose outputs are a fixed offset away from the node
 * coordinates: out(r,g,b) = (r+0.1, g+0.2, b+0.3), clipped nowhere since
 * all values stay under 1.
 */
static const char *offset_cube =
	"TITLE \"offset\"\n"
	"# a comment line\n"
	"LUT_3D_SIZE 2\n"
	"\n"
	"0.1 0.2 0.3\n"	 /* (0,0,0) */
	"1.1 0.2 0.3\n"	 /* (1,0,0) */
	"0.1 1.2 0.3\n"	 /* (0,1,0) */
	"1.1 1.2 0.3\n"	 /* (1,1,0) */
	"0.1 0.2 1.3\n"	 /* (0,0,1) */
	"1.1 0.2 1.3\n"	 /* (1,0,1) */
	"0.1 1.2 1.3\n"	 /* (0,1,1) */
	"1.1 1.2 1.3\n"; /* (1,1,1) */

static void
test_apply_offsets(void)
{
	char *path = write_file("offset.cube", offset_cube);
	VipsCubeLUT *lut = vips_cube_lut_new();
	GError *error = NULL;
	float out[3];

	CHECK(vips_cube_lut_load_file(lut, path, &error) == 0);
	CHECK(error == NULL);
	CHECK(g_strcmp0(vips_cube_lut_get_title(lut), "offset") == 0);
	CHECK(vips_cube_lut_get_size(lut) == 2);
	CHECK(vips_cube_lut_get_shaper_size(lut) == 0);

	/* Exact corners: interpolation weights must collapse onto the node.
	 */
	vips_cube_lut_apply(lut, 0, 0, 0, &out[0], &out[1], &out[2]);
	check_rgb("corner 000", out, 0.1f, 0.2f, 0.3f);

	vips_cube_lut_apply(lut, 1, 1, 1, &out[0], &out[1], &out[2]);
	check_rgb("corner 111", out, 1.1f, 1.2f, 1.3f);

	vips_cube_lut_apply(lut, 1, 0, 1, &out[0], &out[1], &out[2]);
	check_rgb("corner 101", out, 1.1f, 0.2f, 1.3f);

	/* Midpoint: average of all 8 corners.
	 */
	vips_cube_lut_apply(lut, 0.5f, 0.5f, 0.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("midpoint", out, 0.6f, 0.7f, 0.8f);

	/* Quarter point: out = input + offset, linearly.
	 */
	vips_cube_lut_apply(lut, 0.25f, 0.5f, 0.75f,
		&out[0], &out[1], &out[2]);
	check_rgb("quarter", out, 0.35f, 0.7f, 1.05f);

	/* Out-of-range inputs clip to the table edge.
	 */
	vips_cube_lut_apply(lut, -1.0f, 2.0f, 0.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("clipped", out, 0.1f, 1.2f, 0.8f);

	vips_cube_lut_unref(lut);
	g_free(path);
}

/* A shaper that doubles every channel, followed by an identity 2x2x2
 * table. Output must therefore be 2x the (domain-mapped) input, clipped
 * to the table's 0..1 cube on the way in.
 */
static const char *shaper_cube =
	"LUT_1D_SIZE 3\n"
	"LUT_3D_SIZE 2\n"
	"0.0 0.0 0.0\n"
	"1.0 1.0 1.0\n"
	"2.0 2.0 2.0\n"
	"0.0 0.0 0.0\n"
	"1.0 0.0 0.0\n"
	"0.0 1.0 0.0\n"
	"1.0 1.0 0.0\n"
	"0.0 0.0 1.0\n"
	"1.0 0.0 1.0\n"
	"0.0 1.0 1.0\n"
	"1.0 1.0 1.0\n";

static void
test_shaper(void)
{
	char *path = write_file("shaper.cube", shaper_cube);
	VipsCubeLUT *lut = vips_cube_lut_new();
	GError *error = NULL;
	float out[3];

	CHECK(vips_cube_lut_load_file(lut, path, &error) == 0);
	CHECK(vips_cube_lut_get_shaper_size(lut) == 3);
	CHECK(vips_cube_lut_get_size(lut) == 2);
	CHECK(vips_cube_lut_get_title(lut) == NULL);

	/* shaper node 0.5 maps to 1.0, identity table leaves it there,
	 * then it clips at the table's top edge.
	 */
	vips_cube_lut_apply(lut, 0.5f, 0.5f, 0.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("shaper node", out, 1.0f, 1.0f, 1.0f);

	/* shaper 0.25 -> 0.5, table passes it through.
	 */
	vips_cube_lut_apply(lut, 0.25f, 0.25f, 0.25f,
		&out[0], &out[1], &out[2]);
	check_rgb("shaper interp", out, 0.5f, 0.5f, 0.5f);

	/* shaper 0.1 -> 0.2.
	 */
	vips_cube_lut_apply(lut, 0.1f, 0.1f, 0.1f,
		&out[0], &out[1], &out[2]);
	check_rgb("shaper tenth", out, 0.2f, 0.2f, 0.2f);

	/* Channels are independent: feed three different inputs and check
	 * each is doubled without cross-talk.
	 */
	vips_cube_lut_apply(lut, 0.1f, 0.25f, 0.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("shaper channels", out, 0.2f, 0.5f, 1.0f);

	/* Shaper output above 1 clips onto the table edge.
	 */
	vips_cube_lut_apply(lut, 0.75f, 0.75f, 0.75f,
		&out[0], &out[1], &out[2]);
	check_rgb("shaper clip", out, 1.0f, 1.0f, 1.0f);

	vips_cube_lut_unref(lut);
	g_free(path);
}

/* DOMAIN_MIN/MAX remaps the input range: domain 0..10 maps onto 0..1,
 * identity table.
 */
static const char *domain_cube =
	"LUT_3D_SIZE 2\n"
	"DOMAIN_MIN 0.0 0.0 0.0\n"
	"DOMAIN_MAX 10.0 10.0 10.0\n"
	"0.0 0.0 0.0\n"
	"1.0 0.0 0.0\n"
	"0.0 1.0 0.0\n"
	"1.0 1.0 0.0\n"
	"0.0 0.0 1.0\n"
	"1.0 0.0 1.0\n"
	"0.0 1.0 1.0\n"
	"1.0 1.0 1.0\n";

static void
test_domain(void)
{
	char *path = write_file("domain.cube", domain_cube);
	VipsCubeLUT *lut = vips_cube_lut_new();
	GError *error = NULL;
	float out[3];

	CHECK(vips_cube_lut_load_file(lut, path, &error) == 0);

	vips_cube_lut_apply(lut, 5.0f, 5.0f, 5.0f,
		&out[0], &out[1], &out[2]);
	check_rgb("domain midpoint", out, 0.5f, 0.5f, 0.5f);

	vips_cube_lut_apply(lut, 10.0f, 0.0f, 2.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("domain edges", out, 1.0f, 0.0f, 0.25f);

	vips_cube_lut_unref(lut);
	g_free(path);
}

/* CRLF line endings, blank lines and comments must parse identically.
 */
static void
test_crlf_and_buffer(void)
{
	static const char *crlf_cube =
		"TITLE \"crlf case\"\r\n"
		"# comment\r\n"
		"\r\n"
		"LUT_3D_SIZE 2\r\n"
		"0.0 0.0 0.0\r\n"
		"1.0 0.0 0.0\r\n"
		"0.0 1.0 0.0\r\n"
		"1.0 1.0 0.0\r\n"
		"0.0 0.0 1.0\r\n"
		"1.0 0.0 1.0\r\n"
		"0.0 1.0 1.0\r\n"
		"1.0 1.0 1.0\r\n";

	VipsCubeLUT *lut = vips_cube_lut_new();
	GError *error = NULL;
	float out[3];

	CHECK(vips_cube_lut_load_buffer(lut, crlf_cube,
		strlen(crlf_cube), &error) == 0);
	CHECK(error == NULL);

	/* Identity table: midpoint maps to itself.
	 */
	vips_cube_lut_apply(lut, 0.5f, 0.5f, 0.5f,
		&out[0], &out[1], &out[2]);
	check_rgb("crlf midpoint", out, 0.5f, 0.5f, 0.5f);

	vips_cube_lut_unref(lut);
}

static void
expect_load_failure(const char *name, const char *text,
	VipsCubeLUTError code)
{
	char *path = write_file(name, text);
	VipsCubeLUT *lut = vips_cube_lut_new();
	GError *error = NULL;

	CHECK(vips_cube_lut_load_file(lut, path, &error) == -1);
	check_error(error, code, name);

	g_clear_error(&error);
	vips_cube_lut_unref(lut);
	g_free(path);
}

static void
test_errors(void)
{
	GError *error = NULL;
	VipsCubeLUT *lut;

	/* Unreadable file -> OPEN.
	 */
	lut = vips_cube_lut_new();
	error = NULL;
	CHECK(vips_cube_lut_load_file(lut,
		    "this-file-does-not-exist.cube", &error) == -1);
	check_error(error, VIPS_CUBE_LUT_ERROR_OPEN, "missing file");
	g_clear_error(&error);
	vips_cube_lut_unref(lut);

	/* Missing LUT_3D_SIZE.
	 */
	expect_load_failure("no-size.cube",
		"0.0 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Unknown keyword.
	 */
	expect_load_failure("unknown-keyword.cube",
		"LUT_3D_SIZE 2\n"
		"BOGUS 4\n"
		"0.0 0.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 1.0 0.0\n"
		"0.0 0.0 1.0\n"
		"1.0 0.0 1.0\n"
		"0.0 1.0 1.0\n"
		"1.0 1.0 1.0\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Malformed size token.
	 */
	expect_load_failure("bad-size.cube",
		"LUT_3D_SIZE two\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Bad DOMAIN arity.
	 */
	expect_load_failure("bad-domain.cube",
		"LUT_3D_SIZE 2\n"
		"DOMAIN_MIN 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Too few data entries.
	 */
	expect_load_failure("short.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"1.0 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_SIZE);

	/* Too many data entries.
	 */
	expect_load_failure("long.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 1.0 0.0\n"
		"0.0 0.0 1.0\n"
		"1.0 0.0 1.0\n"
		"0.0 1.0 1.0\n"
		"1.0 1.0 1.0\n"
		"0.5 0.5 0.5\n",
		VIPS_CUBE_LUT_ERROR_SIZE);

	/* Shaper declared but missing entries.
	 */
	expect_load_failure("short-shaper.cube",
		"LUT_1D_SIZE 4\n"
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"1.0 1.0 1.0\n"
		"2.0 2.0 2.0\n",
		VIPS_CUBE_LUT_ERROR_SIZE);

	/* Illegal data item: not a number.
	 */
	expect_load_failure("nan-entry.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"banana 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_DATA);

	/* Illegal data item: NaN is rejected.
	 */
	expect_load_failure("nan2.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"nan 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_DATA);

	/* Entry with the wrong number of values.
	 */
	expect_load_failure("two-values.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_DATA);

	/* Zero and negative sizes are malformed headers.
	 */
	expect_load_failure("zero-size.cube",
		"LUT_3D_SIZE 0\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);
	expect_load_failure("neg-size.cube",
		"LUT_3D_SIZE -3\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Infinity is rejected as an illegal data item.
	 */
	expect_load_failure("inf.cube",
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"inf 0.0 0.0\n",
		VIPS_CUBE_LUT_ERROR_DATA);

	/* A shaper with no 3D table is a format error.
	 */
	expect_load_failure("only-shaper.cube",
		"LUT_1D_SIZE 4\n"
		"0.0 0.0 0.0\n"
		"1.0 1.0 1.0\n"
		"2.0 2.0 2.0\n"
		"3.0 3.0 3.0\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Unclosed TITLE quote.
	 */
	expect_load_failure("bad-title.cube",
		"TITLE \"unterminated\n"
		"LUT_3D_SIZE 2\n",
		VIPS_CUBE_LUT_ERROR_FORMAT);

	/* Oversized 3D table.
	 */
	{
		char *text = g_strdup_printf("LUT_3D_SIZE %d\n",
			VIPS_CUBE_LUT_MAX_SIZE + 1);

		expect_load_failure("huge.cube", text,
			VIPS_CUBE_LUT_ERROR_SIZE);
		g_free(text);
	}

	/* Oversized shaper.
	 */
	{
		char *text = g_strdup_printf("LUT_1D_SIZE %d\n"
			"LUT_3D_SIZE 2\n",
			VIPS_CUBE_LUT_MAX_SHAPER_SIZE + 1);

		expect_load_failure("huge-shaper.cube", text,
			VIPS_CUBE_LUT_ERROR_SIZE);
		g_free(text);
	}

	/* Empty input.
	 */
	lut = vips_cube_lut_new();
	error = NULL;
	CHECK(vips_cube_lut_load_buffer(lut, "", 0, &error) == -1);
	check_error(error, VIPS_CUBE_LUT_ERROR_FORMAT, "empty buffer");
	g_clear_error(&error);
	vips_cube_lut_unref(lut);

	/* Loading twice is a format error.
	 */
	{
		char *path = write_file("twice.cube", offset_cube);

		lut = vips_cube_lut_new();
		CHECK(vips_cube_lut_load_file(lut, path, NULL) == 0);
		error = NULL;
		CHECK(vips_cube_lut_load_file(lut, path, &error) == -1);
		check_error(error, VIPS_CUBE_LUT_ERROR_FORMAT, "double load");
		g_clear_error(&error);
		vips_cube_lut_unref(lut);
		g_free(path);
	}
}

/* Exercise the cubelut vips operation end to end.
 */
static void
test_operation(void)
{
	char *path = write_file("op.cube", offset_cube);
	VipsImage *black;
	VipsImage *grad;
	VipsImage *out;
	VipsImage *out2;
	VipsCubeLUT *lut;

	/* A 3-band float image of a known pixel.
	 */
	if (vips_black(&black, 1, 1, "bands", 3, NULL))
		vips_error_exit(NULL);
	if (vips_linear1(black, &grad, 1.0, 0.25, NULL))
		vips_error_exit(NULL);
	g_object_unref(black);
	CHECK(grad->BandFmt == VIPS_FORMAT_FLOAT);

	if (vips_cubelut(grad, &out, "filename", path, NULL))
		vips_error_exit(NULL);

	CHECK(out->BandFmt == VIPS_FORMAT_FLOAT);
	CHECK(out->Bands == 3);

	/* 0.25 + (0.1, 0.2, 0.3).
	 */
	{
		double *pixel = get_pixel(out);

		CHECK(fabs(pixel[0] - 0.35) < 1e-6);
		CHECK(fabs(pixel[1] - 0.45) < 1e-6);
		CHECK(fabs(pixel[2] - 0.55) < 1e-6);
		g_free(pixel);
	}
	g_object_unref(out);

	/* 4-band input: alpha (band 3) passes through untouched.
	 */
	{
		VipsImage *black4;
		VipsImage *grad4;
		VipsImage *out4;

		if (vips_black(&black4, 1, 1, "bands", 4, NULL))
			vips_error_exit(NULL);
		if (vips_linear1(black4, &grad4, 1.0, 0.25, NULL))
			vips_error_exit(NULL);
		g_object_unref(black4);
		if (vips_cubelut(grad4, &out4, "filename", path, NULL))
			vips_error_exit(NULL);
		CHECK(out4->Bands == 4);

		{
			double *pixel = get_pixel(out4);

			CHECK(fabs(pixel[0] - 0.35) < 1e-6);
			CHECK(fabs(pixel[3] - 0.25) < 1e-6);
			g_free(pixel);
		}
		g_object_unref(out4);
		g_object_unref(grad4);
	}

	/* Pre-parsed cube pointer gives the same answer.
	 */
	lut = vips_cube_lut_new();
	if (vips_cube_lut_load_file(lut, path, NULL))
		vips_error_exit(NULL);
	if (vips_cubelut(grad, &out2, "cube", lut, NULL))
		vips_error_exit(NULL);
	{
		double *pixel = get_pixel(out2);

		CHECK(fabs(pixel[0] - 0.35) < 1e-6);
		CHECK(fabs(pixel[1] - 0.45) < 1e-6);
		CHECK(fabs(pixel[2] - 0.55) < 1e-6);
		g_free(pixel);
	}
	g_object_unref(out2);

	/* The borrowed cube is still valid after the operation is destroyed:
	 * reuse it a second time and apply it directly as well.
	 */
	if (vips_cubelut(grad, &out2, "cube", lut, NULL))
		vips_error_exit(NULL);
	{
		double *pixel = get_pixel(out2);

		CHECK(fabs(pixel[0] - 0.35) < 1e-6);
		g_free(pixel);
	}
	g_object_unref(out2);
	{
		float direct[3];

		vips_cube_lut_apply(lut, 0.25f, 0.25f, 0.25f,
			&direct[0], &direct[1], &direct[2]);
		CHECK(fabs(direct[0] - 0.35) < 1e-6);
	}
	vips_cube_lut_unref(lut);

	/* Neither argument set -> operation error.
	 */
	CHECK(vips_cubelut(grad, &out, NULL) != 0);
	vips_error_clear();

	/* Bad filename -> operation error, and the vips error buffer
	 * contains the file name.
	 */
	CHECK(vips_cubelut(grad, &out,
		    "filename", "nope.cube", NULL) != 0);
	{
		const char *text = vips_error_buffer();

		CHECK(strstr(text, "nope.cube") != NULL);
		vips_error_clear();
	}

	g_object_unref(grad);
	g_free(path);
}

/* maplut's existing 1D behaviour must be unchanged: a one-band uchar image
 * mapped through an identity LUT comes back identical.
 */
static void
test_maplut_unchanged(void)
{
	VipsImage *identity;
	VipsImage *index;
	VipsImage *out;
	VipsImage *diff;
	double maxdiff;

	if (vips_identity(&identity, NULL))
		vips_error_exit(NULL);

	/* 1-band uchar index image holding a 0..255 ramp.
	 */
	{
		unsigned char data[256];

		for (int i = 0; i < 256; i++)
			data[i] = (unsigned char) i;
		index = vips_image_new_from_memory_copy(data,
			sizeof(data), 256, 1, 1, VIPS_FORMAT_UCHAR);
		CHECK(index != NULL);
	}

	if (vips_maplut(index, &out, identity, NULL))
		vips_error_exit(NULL);

	if (vips_subtract(index, out, &diff, NULL))
		vips_error_exit(NULL);
	if (vips_max(diff, &maxdiff, NULL))
		vips_error_exit(NULL);

	CHECK(maxdiff == 0.0);

	g_object_unref(diff);
	g_object_unref(out);
	g_object_unref(index);
	g_object_unref(identity);
}

static void
test_refcounting(void)
{
	VipsCubeLUT *lut = vips_cube_lut_new();
	VipsCubeLUT *alias;

	CHECK(vips_cube_lut_get_type() != G_TYPE_INVALID);

	alias = vips_cube_lut_ref(lut);
	CHECK(alias == lut);
	vips_cube_lut_unref(alias);

	/* NULL unref is tolerated.
	 */
	vips_cube_lut_unref(NULL);

	vips_cube_lut_unref(lut);
}

int
main(int argc, char **argv)
{
	if (VIPS_INIT(argv[0]))
		vips_error_exit(NULL);

	if (!vips_type_find("VipsOperation", "cubelut"))
		vips_error_exit("cubelut operation not registered");

	test_apply_offsets();
	test_shaper();
	test_domain();
	test_crlf_and_buffer();
	test_errors();
	test_operation();
	test_maplut_unchanged();
	test_refcounting();

	printf("\n%d checks, %d failures\n", checks, failures);

	if (failures)
		return 1;

	return 0;
}
