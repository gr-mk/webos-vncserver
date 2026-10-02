#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture.h"

// Color encoding of captured video frames
typedef enum {
	VIDEO_COLOR_BT709 = 0,  // BT.709 YCbCr, SDR
	VIDEO_COLOR_BT2020,     // BT.2020 YCbCr, SDR (HDR video tone mapped by TV)
	VIDEO_COLOR_BT2020_PQ,  // BT.2020 YCbCr, PQ transfer (HDR10, Dolby Vision)
	VIDEO_COLOR_BT2020_HLG, // BT.2020 YCbCr, HLG transfer
} video_color_t;

// Large frames are converted by up to this many threads
#define COMPOSITOR_MAX_THREADS 3

// Conversion buffers, one set per thread
typedef struct {
	// Cached copies of currently processed source rows - video buffers are
	// usually mapped uncached, where byte-sized reads are extremely slow.
	uint8_t* lines[2];
	size_t line_capacity;

	// Current source row converted to BGRX
	uint32_t* row;
	size_t row_capacity;
} compositor_scratch_t;

typedef struct {
	uint32_t width;
	uint32_t height;

	// Use NEON accelerated conversion
	bool neon;

	// Output column -> source column lookup table, recomputed on source
	// width change
	uint32_t* xmap;
	uint32_t xmap_src_width;

	// Output column -> UI layer column lookup table, for UI layers of lower
	// resolution than output
	uint32_t* ui_xmap;
	uint32_t ui_xmap_width;

	compositor_scratch_t scratch[COMPOSITOR_MAX_THREADS];

	// HDR -> SDR lookup tables, built on first use for a given transfer:
	// non-linear -> linear light (fixed point, 4096 = SDR white), and linear
	// light -> tone mapped, sRGB encoded output
	video_color_t lut_color;
	uint16_t* eotf;
	uint8_t* oetf;
	// Linear light BT.2020 -> BT.709 matrix, Q12 fixed point
	int32_t gamut[3][3];
} compositor_t;

// Whether CPU supports NEON instructions
bool cpu_has_neon();

int compositor_init(compositor_t* comp, uint32_t width, uint32_t height);
void compositor_destroy(compositor_t* comp);

// Convert video frame (scaled to fit) into BGRX target, blending BGRA UI layer
// (premultiplied alpha, ui_width x ui_height - scaled to fit too) over it,
// unless ui is NULL.
int compositor_run(compositor_t* comp, uint8_t* target, const uint8_t* ui, uint32_t ui_width, uint32_t ui_height,
	const video_frame_t* video, video_color_t color);
