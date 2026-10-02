#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef __arm__
#include <arm_neon.h>
#include "shim/sys/auxv.h"
#endif

#include "compositor.h"

// Fixed point precision of YCbCr -> RGB matrices
#define MATRIX_SHIFT 10

// HDR transfer lookup tables: non-linear R'G'B' (12 bit) -> linear light
// relative to SDR white (fixed point, OETF_SCALE = SDR white) -> tone mapped,
// sRGB encoded output.
#define EOTF_BITS 12
#define EOTF_SIZE (1 << EOTF_BITS)
#define OETF_SCALE 4096
#define OETF_SIZE (8 * OETF_SCALE)
// Fixed point precision of linear light gamut conversion matrix
#define GAMUT_SHIFT 12

// Frames with at least this many pixels are converted by multiple threads
#define COMPOSITOR_PARALLEL_PIXELS (2560 * 1440)

// Luminance mapped to SDR white
#define PQ_REFERENCE_WHITE 100.0
#define HLG_REFERENCE_WHITE 203.0
#define HLG_PEAK 1000.0
// Tone mapping knee, as fraction of SDR white
#define TONEMAP_KNEE 0.75

// YCbCr -> R'G'B' matrices, rows: R, G, B; columns: Cb, Cr
static const double bt709_matrix[3][2] = {
	{0.0, 1.5748},
	{-0.187324, -0.468124},
	{1.8556, 0.0},
};

static const double bt2020_matrix[3][2] = {
	{0.0, 1.4746},
	{-0.164553, -0.571353},
	{1.8814, 0.0},
};

// Linear light BT.2020 -> BT.709 primaries
static const float bt2020_to_bt709[3][3] = {
	{1.660491f, -0.587641f, -0.072850f},
	{-0.124550f, 1.132900f, -0.008349f},
	{-0.018151f, -0.100579f, 1.118730f},
};

typedef struct {
	int32_t luma;
	int32_t chroma[3][2];
} yuv_matrix_t;

// Builds fixed point limited range YCbCr -> R'G'B' matrix, with output values
// scaled to [0, max]. Gamut conversion is optionally folded in, applied to
// non-linear values - inaccurate, but good enough and free.
static void build_matrix(yuv_matrix_t* m, const double ycbcr[3][2], const float gamut[3][3], double max) {
	const double luma_scale = max / 219.0;
	const double chroma_scale = max / 224.0;

	m->luma = (int32_t) lround(luma_scale * (1 << MATRIX_SHIFT));

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 2; col++) {
			double coeff = ycbcr[row][col];
			if (gamut != NULL) {
				coeff = 0;
				for (int k = 0; k < 3; k++) {
					coeff += gamut[row][k] * ycbcr[k][col];
				}
			}
			m->chroma[row][col] = (int32_t) lround(coeff * chroma_scale * (1 << MATRIX_SHIFT));
		}
	}
}

static inline uint32_t clamp8(int32_t v) {
	return v < 0 ? 0 : v > 255 ? 255 : (uint32_t) v;
}

static inline uint32_t clamp12(int32_t v) {
	return v < 0 ? 0 : v > EOTF_SIZE - 1 ? EOTF_SIZE - 1 : (uint32_t) v;
}

// x / 255, rounded, for x in [0, 255 * 255]
static inline uint32_t div255(uint32_t x) {
	return (x + 128 + ((x + 128) >> 8)) >> 8;
}

// Blend premultiplied BGRA fg over opaque bg
static inline uint32_t blend(uint32_t fg, uint32_t bg) {
	uint32_t ia = 255 - (fg >> 24);
	uint32_t out = 0xff000000;

	for (int shift = 0; shift < 24; shift += 8) {
		uint32_t c = ((fg >> shift) & 0xff) + div255(((bg >> shift) & 0xff) * ia);
		out |= (c > 255 ? 255 : c) << shift;
	}

	return out;
}

static double pq_eotf(double e) {
	const double m1 = 0.1593017578125, m2 = 78.84375;
	const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	double p = pow(e, 1.0 / m2);
	return 10000.0 * pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

static double hlg_eotf(double e) {
	const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
	double scene = e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
	// Nominal display OOTF, applied per channel
	return HLG_PEAK * pow(scene, 1.2);
}

static double tonemap(double x) {
	if (x <= TONEMAP_KNEE) {
		return x;
	}
	return TONEMAP_KNEE + (1.0 - TONEMAP_KNEE) * (1.0 - exp(-(x - TONEMAP_KNEE) / (1.0 - TONEMAP_KNEE)));
}

static double srgb_oetf(double x) {
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}

static int build_luts(compositor_t* comp, video_color_t color) {
	if (comp->lut_color == color && comp->eotf != NULL) {
		return 0;
	}

	if (comp->eotf == NULL) {
		comp->eotf = malloc(EOTF_SIZE * sizeof(uint16_t));
		comp->oetf = malloc(OETF_SIZE);
		if (comp->eotf == NULL || comp->oetf == NULL) {
			return -1;
		}

		for (int i = 0; i < OETF_SIZE; i++) {
			comp->oetf[i] = (uint8_t) lround(srgb_oetf(tonemap((double) i / OETF_SCALE)) * 255.0);
		}

		for (int row = 0; row < 3; row++) {
			for (int col = 0; col < 3; col++) {
				comp->gamut[row][col] = (int32_t) lroundf(bt2020_to_bt709[row][col] * (1 << GAMUT_SHIFT));
			}
		}
	}

	for (int i = 0; i < EOTF_SIZE; i++) {
		double e = (double) i / (EOTF_SIZE - 1);
		double linear = color == VIDEO_COLOR_BT2020_HLG ? hlg_eotf(e) / HLG_REFERENCE_WHITE : pq_eotf(e) / PQ_REFERENCE_WHITE;
		// Light this far above SDR white is tone mapped to white anyway
		comp->eotf[i] = (uint16_t) fmin(lround(linear * OETF_SCALE), 65535);
	}

	comp->lut_color = color;
	return 0;
}

static inline uint32_t clamp_oetf(int32_t v) {
	return v < 0 ? 0 : v > OETF_SIZE - 1 ? OETF_SIZE - 1 : (uint32_t) v;
}

// Linear light BT.2020 R, G, B (fixed point) -> tone mapped, sRGB encoded
// BT.709 BGRX pixel
static inline uint32_t hdr_pixel(const compositor_t* comp, int32_t r, int32_t g, int32_t b) {
	const int32_t (*m)[3] = comp->gamut;
	const int32_t round = 1 << (GAMUT_SHIFT - 1);

	return 0xff000000
		| ((uint32_t) comp->oetf[clamp_oetf((m[0][0] * r + m[0][1] * g + m[0][2] * b + round) >> GAMUT_SHIFT)] << 16)
		| ((uint32_t) comp->oetf[clamp_oetf((m[1][0] * r + m[1][1] * g + m[1][2] * b + round) >> GAMUT_SHIFT)] << 8)
		| (uint32_t) comp->oetf[clamp_oetf((m[2][0] * r + m[2][1] * g + m[2][2] * b + round) >> GAMUT_SHIFT)];
}

#ifdef __arm__
bool cpu_has_neon() {
	return (getauxval(AT_HWCAP) & HWCAP_ARM_NEON) != 0;
}

static inline int16_t q6(int32_t coeff) {
	return (int16_t) ((coeff + (1 << (MATRIX_SHIFT - 7))) >> (MATRIX_SHIFT - 6));
}

// NEON version of convert_row_matrix, 16 pixels per iteration, using 16 bit
// Q6 fixed point arithmetic (saturation only affects values that would be
// clamped anyway). Returns number of pixels converted.
__attribute__((target("fpu=neon")))
static uint32_t convert_row_matrix_neon(uint32_t* out, const uint8_t* luma, const uint8_t* chroma, uint32_t width,
	uint32_t u_offset, const yuv_matrix_t* m) {
	const int16_t rb = q6(m->chroma[0][0]), rr = q6(m->chroma[0][1]);
	const int16_t gb = q6(m->chroma[1][0]), gr = q6(m->chroma[1][1]);
	const int16_t bb = q6(m->chroma[2][0]), br = q6(m->chroma[2][1]);
	// 255 / 219 in Q7
	const uint8x8_t luma_scale = vdup_n_u8(149);
	const uint8x8_t luma_offset = vdup_n_u8(16);
	const uint8x8_t chroma_offset = vdup_n_u8(128);
	const uint8x8_t alpha = vdup_n_u8(255);
	uint32_t x = 0;

	for (; x + 16 <= width; x += 16) {
		const uint8x16_t y = vld1q_u8(luma + x);
		const uint8x8x2_t c = vld2_u8(chroma + x);
		const uint8x8_t cu = u_offset ? c.val[1] : c.val[0];
		const uint8x8_t cv = u_offset ? c.val[0] : c.val[1];
		const int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(cu, chroma_offset));
		const int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(cv, chroma_offset));

		// Chroma contribution, each value shared by two horizontal pixels
		const int16x8_t r = vmlaq_n_s16(vmulq_n_s16(u, rb), v, rr);
		const int16x8_t g = vmlaq_n_s16(vmulq_n_s16(u, gb), v, gr);
		const int16x8_t b = vmlaq_n_s16(vmulq_n_s16(u, bb), v, br);
		const int16x8x2_t dr = vzipq_s16(r, r);
		const int16x8x2_t dg = vzipq_s16(g, g);
		const int16x8x2_t db = vzipq_s16(b, b);

		const int16x8_t y0 = vreinterpretq_s16_u16(vshrq_n_u16(vmull_u8(vqsub_u8(vget_low_u8(y), luma_offset), luma_scale), 1));
		const int16x8_t y1 = vreinterpretq_s16_u16(vshrq_n_u16(vmull_u8(vqsub_u8(vget_high_u8(y), luma_offset), luma_scale), 1));

		uint8x8x4_t px0, px1;
		px0.val[0] = vqrshrun_n_s16(vqaddq_s16(y0, db.val[0]), 6);
		px0.val[1] = vqrshrun_n_s16(vqaddq_s16(y0, dg.val[0]), 6);
		px0.val[2] = vqrshrun_n_s16(vqaddq_s16(y0, dr.val[0]), 6);
		px0.val[3] = alpha;
		px1.val[0] = vqrshrun_n_s16(vqaddq_s16(y1, db.val[1]), 6);
		px1.val[1] = vqrshrun_n_s16(vqaddq_s16(y1, dg.val[1]), 6);
		px1.val[2] = vqrshrun_n_s16(vqaddq_s16(y1, dr.val[1]), 6);
		px1.val[3] = alpha;

		vst4_u8((uint8_t*) (out + x), px0);
		vst4_u8((uint8_t*) (out + x + 8), px1);
	}

	return x;
}

static inline int16_t q2(int32_t coeff) {
	return (int16_t) ((coeff + (1 << (MATRIX_SHIFT - 3))) >> (MATRIX_SHIFT - 2));
}

// Stores 12 bit R'G'B' channel values of 8 pixels, from Q2 fixed point
__attribute__((target("fpu=neon")))
static inline void store_q2_12bit(uint16_t* dst, int16x8_t luma, int16x8_t chroma) {
	const int16x8_t value = vrshrq_n_s16(vqaddq_s16(luma, chroma), 2);
	vst1q_u16(dst, vreinterpretq_u16_s16(vminq_s16(vmaxq_s16(value, vdupq_n_s16(0)), vdupq_n_s16(EOTF_SIZE - 1))));
}

// Stores OETF table indices of one output channel of 4 pixels, converted from
// linear light BT.2020 R, G, B with gamut conversion matrix row m
__attribute__((target("fpu=neon")))
static inline void store_gamut(uint16_t* dst, int32x4_t r, int32x4_t g, int32x4_t b, const int32_t m[3]) {
	int32x4_t acc = vmulq_n_s32(r, m[0]);
	acc = vmlaq_n_s32(acc, g, m[1]);
	acc = vmlaq_n_s32(acc, b, m[2]);
	vst1_u16(dst, vmin_u16(vqmovun_s32(vrshrq_n_s32(acc, GAMUT_SHIFT)), vdup_n_u16(OETF_SIZE - 1)));
}

// NEON assisted version of convert_row_hdr, 16 pixels per iteration: Y'CbCr ->
// 12 bit R'G'B' (16 bit Q2 fixed point arithmetic) and gamut conversion are
// vectorized, table lookups aren't. Returns number of pixels converted.
__attribute__((target("fpu=neon")))
static uint32_t convert_row_hdr_neon(const compositor_t* comp, uint32_t* out, const uint8_t* luma,
	const uint8_t* chroma, uint32_t width, uint32_t u_offset, const yuv_matrix_t* m) {
	const int16_t rb = q2(m->chroma[0][0]), rr = q2(m->chroma[0][1]);
	const int16_t gb = q2(m->chroma[1][0]), gr = q2(m->chroma[1][1]);
	const int16_t bb = q2(m->chroma[2][0]), br = q2(m->chroma[2][1]);
	const uint8x8_t luma_scale = vdup_n_u8((uint8_t) q2(m->luma));
	const uint8x8_t luma_offset = vdup_n_u8(16);
	const uint8x8_t chroma_offset = vdup_n_u8(128);
	const uint16_t* eotf = comp->eotf;
	const uint8_t* oetf = comp->oetf;
	uint16_t rgb[3][16] __attribute__((aligned(16)));
	uint16_t lin[3][16] __attribute__((aligned(16)));
	uint16_t idx[3][16] __attribute__((aligned(16)));
	uint32_t x = 0;

	for (; x + 16 <= width; x += 16) {
		const uint8x16_t y = vld1q_u8(luma + x);
		const uint8x8x2_t c = vld2_u8(chroma + x);
		const uint8x8_t cu = u_offset ? c.val[1] : c.val[0];
		const uint8x8_t cv = u_offset ? c.val[0] : c.val[1];
		const int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(cu, chroma_offset));
		const int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(cv, chroma_offset));

		// Chroma contribution, each value shared by two horizontal pixels
		const int16x8_t cr = vmlaq_n_s16(vmulq_n_s16(u, rb), v, rr);
		const int16x8_t cg = vmlaq_n_s16(vmulq_n_s16(u, gb), v, gr);
		const int16x8_t cb = vmlaq_n_s16(vmulq_n_s16(u, bb), v, br);
		const int16x8x2_t dr = vzipq_s16(cr, cr);
		const int16x8x2_t dg = vzipq_s16(cg, cg);
		const int16x8x2_t db = vzipq_s16(cb, cb);

		const int16x8_t y0 = vreinterpretq_s16_u16(vmull_u8(vqsub_u8(vget_low_u8(y), luma_offset), luma_scale));
		const int16x8_t y1 = vreinterpretq_s16_u16(vmull_u8(vqsub_u8(vget_high_u8(y), luma_offset), luma_scale));

		store_q2_12bit(rgb[0], y0, dr.val[0]);
		store_q2_12bit(rgb[0] + 8, y1, dr.val[1]);
		store_q2_12bit(rgb[1], y0, dg.val[0]);
		store_q2_12bit(rgb[1] + 8, y1, dg.val[1]);
		store_q2_12bit(rgb[2], y0, db.val[0]);
		store_q2_12bit(rgb[2] + 8, y1, db.val[1]);

		for (int i = 0; i < 16; i++) {
			lin[0][i] = eotf[rgb[0][i]];
			lin[1][i] = eotf[rgb[1][i]];
			lin[2][i] = eotf[rgb[2][i]];
		}

		for (int i = 0; i < 16; i += 4) {
			const int32x4_t r = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(lin[0] + i)));
			const int32x4_t g = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(lin[1] + i)));
			const int32x4_t b = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(lin[2] + i)));
			store_gamut(idx[0] + i, r, g, b, comp->gamut[0]);
			store_gamut(idx[1] + i, r, g, b, comp->gamut[1]);
			store_gamut(idx[2] + i, r, g, b, comp->gamut[2]);
		}

		for (int i = 0; i < 16; i++) {
			out[x + i] = 0xff000000 | ((uint32_t) oetf[idx[0][i]] << 16) | ((uint32_t) oetf[idx[1][i]] << 8)
				| (uint32_t) oetf[idx[2][i]];
		}
	}

	return x;
}
#else
bool cpu_has_neon() {
	return false;
}
#endif

static void convert_row_matrix(const compositor_t* comp, uint32_t* out, const uint8_t* luma, const uint8_t* chroma,
	uint32_t width, uint32_t u_offset, const yuv_matrix_t* m) {
	const uint32_t v_offset = u_offset ^ 1;
	uint32_t x = 0;

#ifdef __arm__
	if (comp->neon) {
		x = convert_row_matrix_neon(out, luma, chroma, width, u_offset, m);
	}
#endif

	for (; x < width; x += 2) {
		const int32_t u = chroma[x + u_offset] - 128;
		const int32_t v = chroma[x + v_offset] - 128;
		const int32_t dr = m->chroma[0][0] * u + m->chroma[0][1] * v + (1 << (MATRIX_SHIFT - 1));
		const int32_t dg = m->chroma[1][0] * u + m->chroma[1][1] * v + (1 << (MATRIX_SHIFT - 1));
		const int32_t db = m->chroma[2][0] * u + m->chroma[2][1] * v + (1 << (MATRIX_SHIFT - 1));
		const uint32_t n = x + 1 < width ? 2 : 1;

		for (uint32_t i = 0; i < n; i++) {
			const int32_t c = (luma[x + i] - 16) * m->luma;
			out[x + i] = 0xff000000
				| (clamp8((c + dr) >> MATRIX_SHIFT) << 16)
				| (clamp8((c + dg) >> MATRIX_SHIFT) << 8)
				| clamp8((c + db) >> MATRIX_SHIFT);
		}
	}
}

static void convert_row_hdr(const compositor_t* comp, uint32_t* out, const uint8_t* luma, const uint8_t* chroma,
	uint32_t width, uint32_t u_offset, const yuv_matrix_t* m) {
	const uint32_t v_offset = u_offset ^ 1;
	const uint16_t* eotf = comp->eotf;
	uint32_t x = 0;

#ifdef __arm__
	if (comp->neon) {
		x = convert_row_hdr_neon(comp, out, luma, chroma, width, u_offset, m);
	}
#endif

	for (; x < width; x += 2) {
		const int32_t u = chroma[x + u_offset] - 128;
		const int32_t v = chroma[x + v_offset] - 128;
		const int32_t dr = m->chroma[0][0] * u + m->chroma[0][1] * v + (1 << (MATRIX_SHIFT - 1));
		const int32_t dg = m->chroma[1][0] * u + m->chroma[1][1] * v + (1 << (MATRIX_SHIFT - 1));
		const int32_t db = m->chroma[2][0] * u + m->chroma[2][1] * v + (1 << (MATRIX_SHIFT - 1));
		const uint32_t n = x + 1 < width ? 2 : 1;

		for (uint32_t i = 0; i < n; i++) {
			const int32_t c = (luma[x + i] - 16) * m->luma;
			out[x + i] = hdr_pixel(comp, eotf[clamp12((c + dr) >> MATRIX_SHIFT)], eotf[clamp12((c + dg) >> MATRIX_SHIFT)],
				eotf[clamp12((c + db) >> MATRIX_SHIFT)]);
		}
	}
}

static void convert_row_rgb888(uint32_t* out, const uint8_t* rgb, uint32_t width) {
	for (uint32_t x = 0; x < width; x++, rgb += 3) {
		out[x] = 0xff000000 | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
	}
}

int compositor_init(compositor_t* comp, uint32_t width, uint32_t height) {
	memset(comp, 0, sizeof(*comp));
	comp->width = width;
	comp->height = height;
	comp->xmap = calloc(width, sizeof(uint32_t));
	comp->neon = cpu_has_neon();

	return comp->xmap == NULL ? -1 : 0;
}

void compositor_destroy(compositor_t* comp) {
	free(comp->xmap);
	free(comp->ui_xmap);
	for (int i = 0; i < COMPOSITOR_MAX_THREADS; i++) {
		free(comp->scratch[i].lines[0]);
		free(comp->scratch[i].lines[1]);
		free(comp->scratch[i].row);
	}
	free(comp->eotf);
	free(comp->oetf);
	memset(comp, 0, sizeof(*comp));
}

static int reserve(void** buffer, size_t* capacity, size_t size) {
	if (*capacity >= size) {
		return 0;
	}

	void* ptr = realloc(*buffer, size);
	if (ptr == NULL) {
		return -1;
	}

	*buffer = ptr;
	*capacity = size;
	return 0;
}

static int reserve_scratch(compositor_scratch_t* scratch, size_t line_size, size_t row_size) {
	if (scratch->line_capacity < line_size) {
		for (int i = 0; i < 2; i++) {
			uint8_t* line = realloc(scratch->lines[i], line_size);
			if (line == NULL) {
				return -1;
			}
			scratch->lines[i] = line;
		}
		scratch->line_capacity = line_size;
	}

	return reserve((void**) &scratch->row, &scratch->row_capacity, row_size);
}

// Parameters of a single compositor_run() call
typedef struct {
	const compositor_t* comp;
	uint8_t* target;
	const uint8_t* ui;
	uint32_t ui_width;
	uint32_t ui_height;
	const video_frame_t* video;
	yuv_matrix_t matrix;
	bool hdr;
	size_t row_bytes[2];
} compositor_job_t;

// Rows of target converted by a single thread
typedef struct {
	const compositor_job_t* job;
	compositor_scratch_t* scratch;
	uint32_t y_start;
	uint32_t y_end;
} compositor_slice_t;

static void* compositor_run_slice(void* arg) {
	const compositor_slice_t* slice = (const compositor_slice_t*) arg;
	const compositor_job_t* job = slice->job;
	const compositor_t* comp = job->comp;
	const video_frame_t* video = job->video;
	compositor_scratch_t* scratch = slice->scratch;
	const uint8_t* ui = job->ui;
	const uint32_t width = comp->width;
	const uint32_t height = comp->height;
	const uint32_t src_width = video->width;
	const uint32_t src_height = video->height;
	const uint32_t ui_width = job->ui_width;
	const uint32_t ui_height = job->ui_height;
	const bool ui_same_size = ui_width == width && ui_height == height;
	const uint32_t* xmap = comp->xmap;
	const uint32_t* ui_xmap = comp->ui_xmap;
	const uint32_t* row = scratch->row;
	const bool same_width = src_width == width;
	// Rows can be converted straight into target, if they don't need to be
	// scaled or blended
	const bool direct = same_width && ui == NULL;
	const uint32_t u_offset = video->format == VIDEO_FORMAT_NV21 ? 1 : 0;
	int64_t last_row = -1;
	int64_t last_chroma_row = -1;

	for (uint32_t y = slice->y_start; y < slice->y_end; y++) {
		uint32_t* out_row = (uint32_t*) (job->target + (size_t) y * width * 4);
		const uint32_t sy = (uint32_t) (((uint64_t) (2 * y + 1) * src_height) / (2 * height));

		if (sy != last_row) {
			uint32_t* dst = direct ? out_row : scratch->row;

			memcpy(scratch->lines[0], video->planes[0] + (size_t) sy * video->strides[0], job->row_bytes[0]);

			if (video->format == VIDEO_FORMAT_RGB888) {
				convert_row_rgb888(dst, scratch->lines[0], src_width);
			} else {
				const uint32_t cy = video->format == VIDEO_FORMAT_NV16 ? sy : sy / 2;
				if (cy != last_chroma_row) {
					memcpy(scratch->lines[1], video->planes[1] + (size_t) cy * video->strides[1], job->row_bytes[1]);
					last_chroma_row = cy;
				}

				if (job->hdr) {
					convert_row_hdr(comp, dst, scratch->lines[0], scratch->lines[1], src_width, u_offset, &job->matrix);
				} else {
					convert_row_matrix(comp, dst, scratch->lines[0], scratch->lines[1], src_width, u_offset, &job->matrix);
				}
			}
		} else if (ui == NULL) {
			// Same source row as previous output row (upscaling)
			memcpy(out_row, out_row - width, width * 4);
			continue;
		}
		last_row = sy;

		if (ui == NULL) {
			if (!direct) {
				for (uint32_t x = 0; x < width; x++) {
					out_row[x] = row[xmap[x]];
				}
			}
			continue;
		}

		const uint32_t ui_y = ui_same_size ? y : (uint32_t) (((uint64_t) (2 * y + 1) * ui_height) / (2 * height));
		const uint32_t* ui_row = (const uint32_t*) (ui + (size_t) ui_y * ui_width * 4);
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t fg = ui_row[ui_same_size ? x : ui_xmap[x]];
			const uint32_t alpha = fg >> 24;

			if (alpha == 255) {
				out_row[x] = fg;
			} else {
				const uint32_t bg = row[same_width ? x : xmap[x]];
				out_row[x] = alpha == 0 ? bg : blend(fg, bg);
			}
		}
	}

	return NULL;
}

int compositor_run(compositor_t* comp, uint8_t* target, const uint8_t* ui, uint32_t ui_width, uint32_t ui_height,
	const video_frame_t* video, video_color_t color) {
	const uint32_t width = comp->width;
	const uint32_t height = comp->height;
	const uint32_t src_width = video->width;
	const uint32_t src_height = video->height;
	compositor_job_t job = {
		.comp = comp,
		.target = target,
		.ui = ui,
		.ui_width = ui_width,
		.ui_height = ui_height,
		.video = video,
	};

	if (src_width == 0 || src_height == 0 || video->planes[0] == NULL) {
		return -1;
	}

	switch (video->format) {
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_NV21:
	case VIDEO_FORMAT_NV16:
		if (video->planes[1] == NULL) {
			return -1;
		}
		job.row_bytes[0] = src_width;
		job.row_bytes[1] = (src_width + 1) & ~1u;
		break;
	case VIDEO_FORMAT_RGB888:
		job.row_bytes[0] = src_width * 3;
		break;
	default:
		return -2;
	}

	switch (color) {
	case VIDEO_COLOR_BT2020:
		build_matrix(&job.matrix, bt2020_matrix, bt2020_to_bt709, 255.0);
		break;
	case VIDEO_COLOR_BT2020_PQ:
	case VIDEO_COLOR_BT2020_HLG:
		if (build_luts(comp, color) != 0) {
			return -3;
		}
		build_matrix(&job.matrix, bt2020_matrix, NULL, EOTF_SIZE - 1);
		job.hdr = true;
		break;
	default:
		build_matrix(&job.matrix, bt709_matrix, NULL, 255.0);
		break;
	}

	if (comp->xmap_src_width != src_width) {
		for (uint32_t x = 0; x < width; x++) {
			comp->xmap[x] = (uint32_t) (((uint64_t) (2 * x + 1) * src_width) / (2 * width));
		}
		comp->xmap_src_width = src_width;
	}

	if (ui != NULL && (ui_width != width || ui_height != height)) {
		if (ui_width == 0 || ui_height == 0) {
			return -1;
		}

		if (comp->ui_xmap == NULL && (comp->ui_xmap = calloc(width, sizeof(uint32_t))) == NULL) {
			return -3;
		}

		if (comp->ui_xmap_width != ui_width) {
			for (uint32_t x = 0; x < width; x++) {
				comp->ui_xmap[x] = (uint32_t) (((uint64_t) (2 * x + 1) * ui_width) / (2 * width));
			}
			comp->ui_xmap_width = ui_width;
		}
	}

	// Large frames are split into horizontal slices converted in parallel
	const int threads = (uint64_t) width * height >= COMPOSITOR_PARALLEL_PIXELS ? COMPOSITOR_MAX_THREADS : 1;
	const size_t line_size = job.row_bytes[0] > job.row_bytes[1] ? job.row_bytes[0] : job.row_bytes[1];
	compositor_slice_t slices[COMPOSITOR_MAX_THREADS];
	pthread_t thread_ids[COMPOSITOR_MAX_THREADS];
	bool started[COMPOSITOR_MAX_THREADS] = {false};

	for (int i = 0; i < threads; i++) {
		if (reserve_scratch(&comp->scratch[i], line_size, src_width * sizeof(uint32_t)) != 0) {
			return -3;
		}
		slices[i].job = &job;
		slices[i].scratch = &comp->scratch[i];
		slices[i].y_start = (uint32_t) ((uint64_t) height * i / threads);
		slices[i].y_end = (uint32_t) ((uint64_t) height * (i + 1) / threads);
	}

	for (int i = 1; i < threads; i++) {
		started[i] = pthread_create(&thread_ids[i], NULL, compositor_run_slice, &slices[i]) == 0;
	}

	compositor_run_slice(&slices[0]);

	for (int i = 1; i < threads; i++) {
		if (started[i]) {
			pthread_join(thread_ids[i], NULL);
		} else {
			compositor_run_slice(&slices[i]);
		}
	}

	return 0;
}
