/* Load and apply Adobe .cube 3D LUTs.
 *
 * A .cube file contains an optional 1D shaper LUT (LUT_1D_SIZE), a 3D main
 * table (LUT_3D_SIZE), optional DOMAIN_MIN/DOMAIN_MAX and TITLE keywords,
 * and a flat stream of RGB triples.
 *
 * 17/11/26
 *	- from maplut and the Adobe cube LUT specification
 */

/*

	This file is part of VIPS.

	VIPS is free software; you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
	02110-1301  USA

 */

/*

	These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/internal.h>

#include "pcolour.h"

/* Parse state.
 */
typedef enum {
	VIPS_CUBE_PARSE_HEADER, /* Reading keywords */
	VIPS_CUBE_PARSE_SHAPER, /* Reading shaper triples */
	VIPS_CUBE_PARSE_TABLE	 /* Reading 3D table triples */
} VipsCubeParseState;

struct _VipsCubeLUT {
	/* Reference count.
	 */
	int ref_count;

	/* 3D table side length, 0 until LUT_3D_SIZE has been seen.
	 */
	int size;

	/* Shaper length, 0 if there is no 1D shaper.
	 */
	int shaper_size;

	/* Input domain, default 0..1.
	 */
	float domain_min[3];
	float domain_max[3];

	/* Optional TITLE, or NULL.
	 */
	char *title;

	/* Shaper table, shaper_size * 3 floats, or NULL.
	 */
	float *shaper;

	/* 3D table, size^3 * 3 floats, red index fastest.
	 */
	float *table;
};

G_DEFINE_BOXED_TYPE(VipsCubeLUT, vips_cube_lut,
	vips_cube_lut_ref, vips_cube_lut_unref)

/**
 * vips_cube_lut_error_quark:
 *
 * The quark for errors from [struct@CubeLUT] parsing.
 *
 * Returns: the #GQuark for cube LUT errors
 */
GQuark
vips_cube_lut_error_quark(void)
{
	return g_quark_from_static_string("vips-cube-lut-error-quark");
}

static void
vips_cube_lut_set_error(GError **error, VipsCubeLUTError code,
	const char *fmt, ...)
{
	if (error &&
		!*error) {
		va_list ap;
		char buf[512];

		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);

		g_set_error_literal(error, VIPS_CUBE_LUT_ERROR, code, buf);
	}
}

/**
 * vips_cube_lut_new:
 *
 * Allocate a new, empty [struct@CubeLUT]. Load a file into it with
 * [method@CubeLUT.load_file]. Free with [method@CubeLUT.unref].
 *
 * Returns: (transfer full): a new #VipsCubeLUT
 */
VipsCubeLUT *
vips_cube_lut_new(void)
{
	VipsCubeLUT *lut;

	lut = g_new0(VipsCubeLUT, 1);
	lut->ref_count = 1;

	for (int i = 0; i < 3; i++) {
		lut->domain_min[i] = 0.0f;
		lut->domain_max[i] = 1.0f;
	}

	return lut;
}

/**
 * vips_cube_lut_ref:
 * @lut: cube LUT
 *
 * Add a reference to @lut.
 *
 * Returns: (transfer full): @lut
 */
VipsCubeLUT *
vips_cube_lut_ref(VipsCubeLUT *lut)
{
	if (lut)
		g_atomic_int_inc(&lut->ref_count);

	return lut;
}

/**
 * vips_cube_lut_unref:
 * @lut: (nullable): cube LUT
 *
 * Remove a reference from @lut, freeing it when the count reaches zero.
 */
void
vips_cube_lut_unref(VipsCubeLUT *lut)
{
	if (!lut)
		return;

	if (g_atomic_int_dec_and_test(&lut->ref_count)) {
		g_free(lut->title);
		g_free(lut->shaper);
		g_free(lut->table);
		g_free(lut);
	}
}

/**
 * vips_cube_lut_get_title:
 * @lut: cube LUT
 *
 * Return the TITLE keyword from the .cube file, or %NULL if there was no
 * TITLE. The string is owned by @lut and must not be freed.
 *
 * Returns: (transfer none) (nullable): the title
 */
const char *
vips_cube_lut_get_title(const VipsCubeLUT *lut)
{
	return lut ? lut->title : NULL;
}

/**
 * vips_cube_lut_get_size:
 * @lut: cube LUT
 *
 * Return the LUT_3D_SIZE side length, or 0 before a file has been loaded.
 *
 * Returns: the 3D table side length
 */
int
vips_cube_lut_get_size(const VipsCubeLUT *lut)
{
	return lut ? lut->size : 0;
}

/**
 * vips_cube_lut_get_shaper_size:
 * @lut: cube LUT
 *
 * Return the LUT_1D_SIZE shaper length, or 0 when the .cube file has no
 * shaper.
 *
 * Returns: the 1D shaper length
 */
int
vips_cube_lut_get_shaper_size(const VipsCubeLUT *lut)
{
	return lut ? lut->shaper_size : 0;
}

/* Parse one float in the C locale, rejecting empty strings, trailing junk,
 * NaN and infinity.
 */
static gboolean
vips_cube_lut_parse_float(const char *text, float *out,
	GError **error, int line_no)
{
	char *end;
	double value;

	if (!text ||
		!*text) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_DATA,
			_("line %d: expected a number"), line_no);
		return FALSE;
	}

	value = g_ascii_strtod(text, &end);

	/* Skip only trailing whitespace.
	 */
	while (g_ascii_isspace(*end))
		end++;

	if (end == text ||
		*end != '\0' ||
		!isfinite(value)) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_DATA,
			_("line %d: invalid number \"%s\""), line_no, text);
		return FALSE;
	}

	*out = (float) value;

	return TRUE;
}

/* Parse a size keyword value: a positive integer with no trailing junk.
 */
static gboolean
vips_cube_lut_parse_size(const char *text, int *out,
	GError **error, int line_no)
{
	char *end;
	gint64 value;

	value = g_ascii_strtoll(text, &end, 10);

	while (g_ascii_isspace(*end))
		end++;

	if (end == text ||
		*end != '\0' ||
		value <= 0 ||
		value > INT_MAX) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			_("line %d: invalid size \"%s\""), line_no, text);
		return FALSE;
	}

	*out = (int) value;

	return TRUE;
}

/* Split a line into whitespace-separated tokens in place. Returns the number
 * of tokens; @tokens_out is a NULL-terminated array (free with g_free()).
 */
static int
vips_cube_lut_tokenize(char *line, char ***tokens_out)
{
	GPtrArray *tokens;
	char *p;
	int ntokens;

	tokens = g_ptr_array_new();
	p = line;

	while (*p) {
		while (*p &&
			g_ascii_isspace(*p))
			*p++ = '\0';

		if (!*p)
			break;

		g_ptr_array_add(tokens, p);

		while (*p &&
			!g_ascii_isspace(*p))
			p++;
	}

	ntokens = (int) tokens->len;
	g_ptr_array_add(tokens, NULL);

	*tokens_out = (char **) g_ptr_array_free(tokens, FALSE);

	return ntokens;
}

/* TRUE if @line starts with @keyword, case-insensitively, and the keyword is
 * followed by whitespace or the end of the line.
 */
static gboolean
vips_cube_lut_keyword_is(const char *line, const char *keyword)
{
	size_t len = strlen(keyword);

	return g_ascii_strncasecmp(line, keyword, len) == 0 &&
		(line[len] == '\0' ||
			g_ascii_isspace(line[len]));
}

/* Parse state for one vips_cube_lut_load_buffer() call.
 */
typedef struct _VipsCubeParse {
	VipsCubeLUT *lut;

	VipsCubeParseState state;
	gsize table_capacity;
	gsize shaper_capacity;
	gsize table_entries;
	gsize shaper_entries;
	gboolean seen_data;
} VipsCubeParse;

/* Store one RGB triple at the next shaper or table slot.
 */
static gboolean
vips_cube_lut_store_triple(VipsCubeParse *parse, char **tokens, int ntokens,
	int line_no, GError **error)
{
	VipsCubeLUT *lut = parse->lut;
	float rgb[3];
	float *target;
	gsize capacity;
	gsize index;

	if (ntokens != 3) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_DATA,
			_("line %d: data entries must have exactly 3 values"),
			line_no);
		return FALSE;
	}

	for (int i = 0; i < 3; i++)
		if (!vips_cube_lut_parse_float(tokens[i], &rgb[i],
			    error, line_no))
			return FALSE;

	if (parse->state == VIPS_CUBE_PARSE_SHAPER) {
		capacity = parse->shaper_capacity;
		index = parse->shaper_entries;
	}
	else {
		capacity = parse->table_capacity;
		index = parse->table_entries;
	}

	if (index >= capacity) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_SIZE,
			_("line %d: too many data entries "
				  "(expected %" G_GSIZE_FORMAT
				  " RGB triples)"),
			line_no, capacity);
		return FALSE;
	}

	target = parse->state == VIPS_CUBE_PARSE_SHAPER ?
		&lut->shaper[index * 3] :
		&lut->table[index * 3];

	target[0] = rgb[0];
	target[1] = rgb[1];
	target[2] = rgb[2];

	return TRUE;
}

/* Allocate the shaper and table arrays on first use.
 */
static gboolean
vips_cube_lut_alloc_tables(VipsCubeParse *parse, GError **error)
{
	VipsCubeLUT *lut = parse->lut;

	parse->table_capacity = (gsize) lut->size * lut->size * lut->size;
	parse->shaper_capacity = (gsize) lut->shaper_size;

	if (!(lut->table = g_try_malloc_n(parse->table_capacity, 3 * sizeof(float)))) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_SIZE,
			_("3D table of %d^3 entries is too large to allocate"),
			lut->size);
		return FALSE;
	}

	if (parse->shaper_capacity > 0 &&
		!(lut->shaper = g_try_malloc_n(parse->shaper_capacity,
			  3 * sizeof(float)))) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_SIZE,
			_("shaper table of %d entries is too large to allocate"),
			lut->shaper_size);
		return FALSE;
	}

	return TRUE;
}

/* Handle a size keyword. @shaper selects LUT_1D_SIZE.
 */
static gboolean
vips_cube_lut_handle_size(VipsCubeParse *parse, const char *keyword,
	char **tokens, int ntokens, gboolean shaper,
	int line_no, GError **error)
{
	VipsCubeLUT *lut = parse->lut;
	int size;
	int maximum;

	if (parse->state != VIPS_CUBE_PARSE_HEADER ||
		(shaper ? lut->shaper_size != 0 : lut->size != 0)) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			_("line %d: duplicate or misplaced %s"),
			line_no, keyword);
		return FALSE;
	}

	if (ntokens != 2) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			_("line %d: %s needs one value"), line_no, keyword);
		return FALSE;
	}

	if (!vips_cube_lut_parse_size(tokens[1], &size, error, line_no))
		return FALSE;

	maximum = shaper ?
		VIPS_CUBE_LUT_MAX_SHAPER_SIZE :
		VIPS_CUBE_LUT_MAX_SIZE;
	if (size > maximum) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_SIZE,
			_("line %d: %s %d is larger than the maximum of %d"),
			line_no, keyword, size, maximum);
		return FALSE;
	}

	if (shaper)
		lut->shaper_size = size;
	else
		lut->size = size;

	return TRUE;
}

/* Handle one line. @line is modified in place.
 */
static gboolean
vips_cube_lut_handle_line(VipsCubeParse *parse, char *line, int line_no,
	GError **error)
{
	VipsCubeLUT *lut = parse->lut;
	char *trimmed = line;
	char **tokens = NULL;
	int ntokens;
	gboolean result = TRUE;

	while (*trimmed &&
		g_ascii_isspace(*trimmed))
		trimmed++;

	/* TITLE takes a quoted argument which would be broken up by
	 * tokenizing, so handle it on the raw line.
	 */
	if (vips_cube_lut_keyword_is(trimmed, "TITLE")) {
		char *first_quote;
		char *last_quote;

		if (parse->state != VIPS_CUBE_PARSE_HEADER) {
			vips_cube_lut_set_error(error,
				VIPS_CUBE_LUT_ERROR_FORMAT,
				_("line %d: TITLE must appear before any data"),
				line_no);
			return FALSE;
		}

		first_quote = strchr(trimmed, '"');
		last_quote = strrchr(trimmed, '"');
		if (!first_quote ||
			!last_quote ||
			first_quote == last_quote) {
			vips_cube_lut_set_error(error,
				VIPS_CUBE_LUT_ERROR_FORMAT,
				_("line %d: TITLE must be a quoted string"),
				line_no);
			return FALSE;
		}

		*last_quote = '\0';
		g_free(lut->title);
		lut->title = g_strdup(first_quote + 1);

		return TRUE;
	}

	ntokens = vips_cube_lut_tokenize(line, &tokens);
	if (ntokens == 0)
		goto done;

	{
		const char *keyword = tokens[0];

		if (keyword[0] == '#') {
			/* Comment line, ignore.
			 */
		}
		else if (g_ascii_strcasecmp(keyword, "LUT_3D_SIZE") == 0) {
			if (!vips_cube_lut_handle_size(parse, keyword,
				    tokens, ntokens, FALSE, line_no, error))
				result = FALSE;
		}
		else if (g_ascii_strcasecmp(keyword, "LUT_1D_SIZE") == 0) {
			if (!vips_cube_lut_handle_size(parse, keyword,
				    tokens, ntokens, TRUE, line_no, error))
				result = FALSE;
		}
		else if (g_ascii_strcasecmp(keyword, "DOMAIN_MIN") == 0 ||
			g_ascii_strcasecmp(keyword, "DOMAIN_MAX") == 0) {
			float *target;

			if (parse->state != VIPS_CUBE_PARSE_HEADER) {
				vips_cube_lut_set_error(error,
					VIPS_CUBE_LUT_ERROR_FORMAT,
					_("line %d: domain keywords must appear "
						  "before any data"),
					line_no);
				result = FALSE;
				goto done;
			}

			if (ntokens != 4) {
				vips_cube_lut_set_error(error,
					VIPS_CUBE_LUT_ERROR_FORMAT,
					_("line %d: %s needs 3 values"),
					line_no, keyword);
				result = FALSE;
				goto done;
			}

			target = g_ascii_strcasecmp(keyword, "DOMAIN_MIN") == 0 ?
				lut->domain_min :
				lut->domain_max;

			for (int i = 0; i < 3; i++)
				if (!vips_cube_lut_parse_float(tokens[i + 1],
					    &target[i], error, line_no)) {
					result = FALSE;
					goto done;
				}
		}
		else if (g_ascii_isalpha(keyword[0]) ||
			keyword[0] == '_') {
			/* A word that is not a recognized keyword. Before any
			 * data it is an unknown keyword; once data has
			 * started it can only be an illegal data value.
			 */
			if (parse->state == VIPS_CUBE_PARSE_HEADER) {
				vips_cube_lut_set_error(error,
					VIPS_CUBE_LUT_ERROR_FORMAT,
					_("line %d: unknown keyword \"%s\""),
					line_no, keyword);
				result = FALSE;
				goto done;
			}

			{
				float dummy;

				/* parse_float produces the error message for an
				 * illegal token such as "banana", "nan" or
				 * "inf" (the latter two parse as numbers but
				 * are rejected as non-finite).
				 */
				if (vips_cube_lut_parse_float(keyword, &dummy,
					    error, line_no))
					vips_cube_lut_set_error(error,
						VIPS_CUBE_LUT_ERROR_DATA,
						_("line %d: invalid number "
						  "\"%s\""),
						line_no, keyword);
			}

			result = FALSE;
			goto done;
		}
		else {
			/* A bare number: the data section starts here.
			 */
			if (lut->size == 0) {
				vips_cube_lut_set_error(error,
					VIPS_CUBE_LUT_ERROR_FORMAT,
					_("line %d: data before LUT_3D_SIZE"),
					line_no);
				result = FALSE;
				goto done;
			}

			if (!parse->seen_data) {
				parse->seen_data = TRUE;
				parse->state = lut->shaper_size > 0 ?
					VIPS_CUBE_PARSE_SHAPER :
					VIPS_CUBE_PARSE_TABLE;

				if (!vips_cube_lut_alloc_tables(parse, error)) {
					result = FALSE;
					goto done;
				}
			}

			if (!vips_cube_lut_store_triple(parse, tokens, ntokens,
				    line_no, error)) {
				result = FALSE;
				goto done;
			}

			if (parse->state == VIPS_CUBE_PARSE_SHAPER) {
				parse->shaper_entries++;
				if (parse->shaper_entries ==
					parse->shaper_capacity)
					parse->state = VIPS_CUBE_PARSE_TABLE;
			}
			else
				parse->table_entries++;
		}
	}

done:
	g_free(tokens);

	return result;
}

/**
 * vips_cube_lut_load_buffer:
 * @lut: an empty #VipsCubeLUT from [ctor@CubeLUT.new]
 * @data: (array length=length): .cube file contents
 * @length: length of @data in bytes
 * @error: return location for a #GError
 *
 * Parse Adobe .cube file contents into @lut. @lut must be freshly
 * allocated; loading twice is not supported.
 *
 * The error code distinguishes unreadable files
 * ([error@CubeLUTError.OPEN]), malformed headers
 * ([error@CubeLUTError.FORMAT]), size mismatches or oversized tables
 * ([error@CubeLUTError.SIZE]) and invalid data entries
 * ([error@CubeLUTError.DATA]).
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_cube_lut_load_buffer(VipsCubeLUT *lut, const void *data, size_t length,
	GError **error)
{
	char *copy;
	char *line;
	char *next;
	int line_no;
	VipsCubeParse parse;

	g_return_val_if_fail(lut != NULL, -1);

	if (lut->size != 0 ||
		lut->shaper_size != 0 ||
		lut->table ||
		lut->shaper) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			"%s", _("cube LUT has already been loaded"));
		return -1;
	}

	if (!data ||
		length == 0) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			"%s", _("empty .cube file"));
		return -1;
	}

	/* A NUL-terminated private copy, so we can tokenize in place.
	 */
	copy = g_malloc(length + 1);
	memcpy(copy, data, length);
	copy[length] = '\0';

	memset(&parse, 0, sizeof(parse));
	parse.lut = lut;
	parse.state = VIPS_CUBE_PARSE_HEADER;

	line_no = 0;
	for (line = copy; line; line = next) {
		size_t len;

		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		line_no++;

		/* Strip a trailing CR from CRLF line endings.
		 */
		len = strlen(line);
		if (len > 0 &&
			line[len - 1] == '\r')
			line[len - 1] = '\0';

		if (!vips_cube_lut_handle_line(&parse, line, line_no, error)) {
			g_free(copy);
			return -1;
		}
	}

	if (lut->size == 0) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			"%s", _("no LUT_3D_SIZE in .cube file"));
		g_free(copy);
		return -1;
	}

	if (!parse.seen_data) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_FORMAT,
			"%s", _("no data entries in .cube file"));
		g_free(copy);
		return -1;
	}

	if (parse.table_entries != parse.table_capacity ||
		parse.shaper_entries != parse.shaper_capacity) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_SIZE,
			_("expected %" G_GSIZE_FORMAT " shaper and "
			  "%" G_GSIZE_FORMAT " table RGB triples but "
			  "found %" G_GSIZE_FORMAT " and %" G_GSIZE_FORMAT),
			parse.shaper_capacity, parse.table_capacity,
			parse.shaper_entries, parse.table_entries);
		g_free(copy);
		return -1;
	}

	g_free(copy);

	return 0;
}

/**
 * vips_cube_lut_load_file:
 * @lut: an empty #VipsCubeLUT from [ctor@CubeLUT.new]
 * @filename: path to a .cube file
 * @error: return location for a #GError
 *
 * Load and parse a .cube file into @lut. See
 * [method@CubeLUT.load_buffer] for the error codes.
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_cube_lut_load_file(VipsCubeLUT *lut, const char *filename,
	GError **error)
{
	size_t length;
	char *data;
	int result;

	g_return_val_if_fail(lut != NULL, -1);
	g_return_val_if_fail(filename != NULL, -1);

	if (!(data = vips__file_read_name(filename, NULL, &length))) {
		vips_cube_lut_set_error(error, VIPS_CUBE_LUT_ERROR_OPEN,
			_("unable to read \"%s\""), filename);
		return -1;
	}

	result = vips_cube_lut_load_buffer(lut, data, length, error);

	g_free(data);

	return result;
}

/* Linear interpolation over one channel of the 1D shaper. The shaper is
 * stored as RGB triples, but each channel is interpolated independently.
 */
static float
vips_cube_lut_interp_shaper(const float *shaper, int shaper_size,
	int band, float x)
{
	float position;
	int low;
	int high;
	float fraction;

	if (shaper_size <= 1 ||
		x <= 0.0f)
		return shaper[band];

	if (x >= 1.0f)
		return shaper[(shaper_size - 1) * 3 + band];

	position = x * (shaper_size - 1);
	low = (int) floor(position);
	high = low + 1;
	fraction = position - low;

	return shaper[low * 3 + band] * (1.0f - fraction) +
		shaper[high * 3 + band] * fraction;
}

/* Trilinear interpolation in the 3D table. The table is stored with the
 * red index changing fastest, matching the .cube layout.
 */
static void
vips_cube_lut_interp_table(const VipsCubeLUT *lut,
	float r, float g, float b,
	float *out_r, float *out_g, float *out_b)
{
	int n = lut->size;
	float pos[3];
	int low[3];
	int high[3];
	float frac[3];
	float result[3] = { 0.0f, 0.0f, 0.0f };

	pos[0] = VIPS_CLIP(0.0f, r, 1.0f) * (n - 1);
	pos[1] = VIPS_CLIP(0.0f, g, 1.0f) * (n - 1);
	pos[2] = VIPS_CLIP(0.0f, b, 1.0f) * (n - 1);

	for (int i = 0; i < 3; i++) {
		low[i] = (int) floor(pos[i]);
		high[i] = low[i] + 1;
		if (high[i] >= n)
			high[i] = n - 1;
		frac[i] = pos[i] - low[i];
	}

	/* Walk the 8 cube corners.
	 */
	for (int corner = 0; corner < 8; corner++) {
		int ri = (corner & 1) ? high[0] : low[0];
		int gi = (corner & 2) ? high[1] : low[1];
		int bi = (corner & 4) ? high[2] : low[2];
		float weight;
		int index;

		weight = ((corner & 1) ? frac[0] : 1.0f - frac[0]) *
			((corner & 2) ? frac[1] : 1.0f - frac[1]) *
			((corner & 4) ? frac[2] : 1.0f - frac[2]);

		index = (bi * n + gi) * n + ri;

		for (int band = 0; band < 3; band++)
			result[band] +=
				weight * lut->table[index * 3 + band];
	}

	*out_r = result[0];
	*out_g = result[1];
	*out_b = result[2];
}

/**
 * vips_cube_lut_apply:
 * @lut: a loaded #VipsCubeLUT
 * @r: red input value
 * @g: green input value
 * @b: blue input value
 * @out_r: (out): transformed red value
 * @out_g: (out): transformed green value
 * @out_b: (out): transformed blue value
 *
 * Apply @lut to one RGB pixel. The input is mapped from the declared
 * domain, passed through the 1D shaper (if present) and then through the
 * 3D table with trilinear interpolation.
 */
void
vips_cube_lut_apply(const VipsCubeLUT *lut,
	float r, float g, float b,
	float *out_r, float *out_g, float *out_b)
{
	float rgb[3];

	g_return_if_fail(lut != NULL);
	g_return_if_fail(lut->table != NULL);
	g_return_if_fail(out_r != NULL);
	g_return_if_fail(out_g != NULL);
	g_return_if_fail(out_b != NULL);

	rgb[0] = r;
	rgb[1] = g;
	rgb[2] = b;

	/* Map from the declared domain onto the 0..1 unit cube.
	 */
	for (int i = 0; i < 3; i++) {
		float range = lut->domain_max[i] - lut->domain_min[i];

		if (range != 0.0f)
			rgb[i] = (rgb[i] - lut->domain_min[i]) / range;
		else
			rgb[i] = 0.0f;

		rgb[i] = VIPS_CLIP(0.0f, rgb[i], 1.0f);
	}

	/* Optional 1D shaper.
	 */
	if (lut->shaper) {
		float shaped[3];

		for (int i = 0; i < 3; i++)
			shaped[i] = vips_cube_lut_interp_shaper(
				lut->shaper, lut->shaper_size, i, rgb[i]);

		rgb[0] = shaped[0];
		rgb[1] = shaped[1];
		rgb[2] = shaped[2];
	}

	vips_cube_lut_interp_table(lut,
		rgb[0], rgb[1], rgb[2], out_r, out_g, out_b);
}

typedef struct _VipsCubelut {
	VipsColourTransform parent_instance;

	char *filename;
	VipsCubeLUT *cube;

	/* TRUE when we hold a reference to @cube.
	 */
	gboolean owns_cube;
} VipsCubelut;

typedef VipsColourTransformClass VipsCubelutClass;

G_DEFINE_TYPE(VipsCubelut, vips_cubelut, VIPS_TYPE_COLOUR_TRANSFORM);

static void
vips_cubelut_line(VipsColour *colour, VipsPel *out, VipsPel **in, int width)
{
	VipsCubelut *cubelut = (VipsCubelut *) colour;
	float *restrict p = (float *) in[0];
	float *restrict q = (float *) out;

	for (int i = 0; i < width; i++) {
		vips_cube_lut_apply(cubelut->cube,
			p[0], p[1], p[2],
			&q[0], &q[1], &q[2]);

		p += 3;
		q += 3;
	}
}

static int
vips_cubelut_build(VipsObject *object)
{
	VipsObjectClass *class = VIPS_OBJECT_GET_CLASS(object);
	VipsCubelut *cubelut = (VipsCubelut *) object;
	VipsColour *colour = (VipsColour *) object;
	VipsColourTransform *transform = (VipsColourTransform *) object;
	GError *error = NULL;

	if (!cubelut->cube &&
		!cubelut->filename) {
		vips_error(class->nickname, "%s",
			_("either filename or cube must be set"));
		return -1;
	}

	if (cubelut->cube) {
		if (!cubelut->cube->table) {
			vips_error(class->nickname, "%s",
				_("cube LUT has not been loaded"));
			return -1;
		}

		/* A cube passed in through the @cube property is borrowed, so
		 * take a reference for as long as we need it.
		 */
		vips_cube_lut_ref(cubelut->cube);
		cubelut->owns_cube = TRUE;
	}
	else {
		cubelut->cube = vips_cube_lut_new();
		cubelut->owns_cube = TRUE;

		if (vips_cube_lut_load_file(cubelut->cube,
			    cubelut->filename, &error)) {
			vips_error(class->nickname,
				_("unable to load \"%s\": %s"),
				cubelut->filename, error->message);
			g_error_free(error);
			return -1;
		}
	}

	/* A 3D LUT is an RGB-to-RGB mapping: keep the input interpretation
	 * and write float. These must be set before the parent build creates
	 * the output image.
	 */
	if (transform->in)
		colour->interpretation = transform->in->Type;
	colour->format = VIPS_FORMAT_FLOAT;
	colour->bands = 3;

	if (VIPS_OBJECT_CLASS(vips_cubelut_parent_class)->build(object))
		return -1;

	return 0;
}

static void
vips_cubelut_finalize(GObject *object)
{
	VipsCubelut *cubelut = (VipsCubelut *) object;

	if (cubelut->owns_cube) {
		vips_cube_lut_unref(cubelut->cube);
		cubelut->cube = NULL;
		cubelut->owns_cube = FALSE;
	}

	G_OBJECT_CLASS(vips_cubelut_parent_class)->finalize(object);
}

static void
vips_cubelut_class_init(VipsCubelutClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsColourClass *colour_class = VIPS_COLOUR_CLASS(class);

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;
	gobject_class->finalize = vips_cubelut_finalize;

	object_class->nickname = "cubelut";
	object_class->description = _("apply a .cube 3D LUT to an image");
	object_class->build = vips_cubelut_build;

	colour_class->process_line = vips_cubelut_line;

	VIPS_ARG_STRING(class, "filename", 10,
		_("Filename"),
		_("Filename of .cube LUT to load"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsCubelut, filename),
		NULL);

	VIPS_ARG_POINTER(class, "cube", 11,
		_("Cube LUT"),
		_("Pre-parsed VipsCubeLUT, alternative to filename"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsCubelut, cube));
}

static void
vips_cubelut_init(VipsCubelut *cubelut)
{
	cubelut->filename = NULL;
	cubelut->cube = NULL;
	cubelut->owns_cube = FALSE;
}

/**
 * vips_cubelut: (method)
 * @in: input image
 * @out: (out): output image
 * @...: `NULL`-terminated list of optional named arguments
 *
 * Apply an Adobe .cube 3D LUT to an image. The image is cast to float and
 * the first three bands are mapped through the LUT with trilinear
 * interpolation; any extra bands (such as alpha) are passed through
 * unchanged.
 *
 * Set @filename to load a .cube file, or pass a pre-parsed
 * [struct@CubeLUT] as @cube to apply the same table repeatedly without
 * reparsing. A .cube file may contain a 1D shaper LUT as well as the 3D
 * main table.
 *
 * ::: tip "Optional arguments"
 *     * @filename: `gchararray`, path to a .cube file
 *     * @cube: `VipsCubeLUT`, a pre-parsed cube LUT
 *
 * ::: seealso
 *     [method@Image.maplut], [ctor@CubeLUT.new].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_cubelut(VipsImage *in, VipsImage **out, ...)
{
	va_list ap;
	int result;

	va_start(ap, out);
	result = vips_call_split("cubelut", ap, in, out);
	va_end(ap);

	return result;
}
