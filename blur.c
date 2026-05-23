/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2009 Chris Wilson
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <cairo.h>
#include "blur.h"

#ifdef __x86_64__
#include <immintrin.h>
#endif

/*
 * Computes three box-blur radii that approximate a Gaussian of standard
 * deviation @sigma via three sequential box-blur passes.
 * Algorithm: P. Kovesi, "Fast Almost-Gaussian Filtering", DICTA 2010.
 *   https://www.peterkovesi.com/papers/FastGaussianSmoothing.pdf
 */
static void boxes_for_gauss(int sigma, int boxes[3]) {
    double w_ideal = sqrt((12.0 * sigma * sigma / 3.0) + 1.0);
    int w_l = (int)w_ideal;
    if (w_l % 2 == 0)
        w_l--; /* round down to nearest odd */
    int w_u = w_l + 2;
    double m_ideal = (12.0 * sigma * sigma - 3.0 * w_l * w_l - 12.0 * w_l - 9.0) /
                     (-4.0 * w_l - 4.0);
    int m = (int)(m_ideal + 0.5); /* round to nearest int */
    for (int i = 0; i < 3; i++)
        boxes[i] = ((i < m ? w_l : w_u) - 1) / 2;
}

/*
 * Scalar horizontal box-blur pass: src → dst.
 * Each row is processed with a sliding window of radius @r;
 * border pixels are clamped (replicated edge).
 */
static void box_blur_hpass_scalar(uint32_t *src, uint32_t *dst,
                                   int w, int h, int r) {
    int diam = 2 * r + 1;

    for (int y = 0; y < h; y++) {
        uint32_t *row_src = src + (ptrdiff_t)y * w;
        uint32_t *row_out = dst + (ptrdiff_t)y * w;
        uint32_t aa = 0, ra = 0, ga = 0, ba = 0;

        /* Prime the sliding window with r clamped-left pixels + pixel [0] */
        for (int x = -r; x <= r; x++) {
            int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
            uint32_t p = row_src[sx];
            aa += (p >> 24) & 0xFF;
            ra += (p >> 16) & 0xFF;
            ga += (p >> 8) & 0xFF;
            ba += p & 0xFF;
        }

        for (int x = 0; x < w; x++) {
            row_out[x] = ((aa / (uint32_t)diam) << 24) |
                         ((ra / (uint32_t)diam) << 16) |
                         ((ga / (uint32_t)diam) << 8) |
                         (ba / (uint32_t)diam);

            int out_x = x - r;
            int in_x = x + r + 1;
            int sx_out = out_x < 0 ? 0 : (out_x >= w ? w - 1 : out_x);
            int sx_in = in_x < 0 ? 0 : (in_x >= w ? w - 1 : in_x);
            uint32_t p_out = row_src[sx_out];
            uint32_t p_in = row_src[sx_in];
            aa += ((p_in >> 24) & 0xFF) - ((p_out >> 24) & 0xFF);
            ra += ((p_in >> 16) & 0xFF) - ((p_out >> 16) & 0xFF);
            ga += ((p_in >> 8) & 0xFF) - ((p_out >> 8) & 0xFF);
            ba += (p_in & 0xFF) - (p_out & 0xFF);
        }
    }
}

/*
 * SSE2 vectorized horizontal box-blur pass: src → dst.
 * Processes one pixel at a time using SSE2 to parallelize the
 * four-channel (ARGB) accumulate / divide / pack.
 */
#ifdef __x86_64__
__attribute__((target("sse2"))) static void
box_blur_hpass_sse2(uint32_t *src, uint32_t *dst, int w, int h, int r) {
    int diam = 2 * r + 1;
    __m128 diam_vec = _mm_set1_ps((float)diam);
    __m128i zero = _mm_setzero_si128();

    for (int y = 0; y < h; y++) {
        uint32_t *row_src = src + (ptrdiff_t)y * w;
        uint32_t *row_out = dst + (ptrdiff_t)y * w;

        /* Prime the sliding window with scalar accumulation */
        __m128i acc = _mm_setzero_si128();
        for (int x = -r; x <= r; x++) {
            int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
            uint32_t p = row_src[sx];
            __m128i pv = _mm_cvtsi32_si128((int)p);
            __m128i unpacked = _mm_unpacklo_epi8(pv, zero);
            unpacked = _mm_unpacklo_epi16(unpacked, zero);
            acc = _mm_add_epi32(acc, unpacked);
        }

        for (int x = 0; x < w; x++) {
            /* Divide accumulator by diameter via float conversion */
            __m128 f = _mm_cvtepi32_ps(acc);
            f = _mm_div_ps(f, diam_vec);
            __m128i result = _mm_cvttps_epi32(f);

            /* Pack back to a single ARGB uint32 */
            result = _mm_packs_epi32(result, zero);
            result = _mm_packus_epi16(result, zero);
            row_out[x] = (uint32_t)_mm_cvtsi128_si32(result);

            /* Slide window: subtract outgoing, add incoming */
            int out_x = x - r;
            int in_x = x + r + 1;
            int sx_out = out_x < 0 ? 0 : (out_x >= w ? w - 1 : out_x);
            int sx_in = in_x < 0 ? 0 : (in_x >= w ? w - 1 : in_x);
            uint32_t p_out = row_src[sx_out];
            uint32_t p_in = row_src[sx_in];

            __m128i pv_out = _mm_cvtsi32_si128((int)p_out);
            __m128i unpacked_out = _mm_unpacklo_epi8(pv_out, zero);
            unpacked_out = _mm_unpacklo_epi16(unpacked_out, zero);

            __m128i pv_in = _mm_cvtsi32_si128((int)p_in);
            __m128i unpacked_in = _mm_unpacklo_epi8(pv_in, zero);
            unpacked_in = _mm_unpacklo_epi16(unpacked_in, zero);

            acc = _mm_sub_epi32(acc, unpacked_out);
            acc = _mm_add_epi32(acc, unpacked_in);
        }
    }
}
#endif /* __x86_64__ */

/*
 * Vertical box-blur pass: scratch → dst.
 * Each column is processed with a sliding window of radius @r.
 */
static void box_blur_vpass(uint32_t *src, uint32_t *dst,
                            int w, int h, int r) {
    int diam = 2 * r + 1;

    for (int x = 0; x < w; x++) {
        uint32_t aa = 0, ra = 0, ga = 0, ba = 0;

        /* Prime the window */
        for (int y = -r; y <= r; y++) {
            int sy = y < 0 ? 0 : (y >= h ? h - 1 : y);
            uint32_t p = src[(ptrdiff_t)sy * w + x];
            aa += (p >> 24) & 0xFF;
            ra += (p >> 16) & 0xFF;
            ga += (p >> 8) & 0xFF;
            ba += p & 0xFF;
        }

        for (int y = 0; y < h; y++) {
            dst[(ptrdiff_t)y * w + x] = ((aa / (uint32_t)diam) << 24) |
                                         ((ra / (uint32_t)diam) << 16) |
                                         ((ga / (uint32_t)diam) << 8) |
                                         (ba / (uint32_t)diam);

            int out_y = y - r;
            int in_y = y + r + 1;
            int sy_out = out_y < 0 ? 0 : (out_y >= h ? h - 1 : out_y);
            int sy_in = in_y < 0 ? 0 : (in_y >= h ? h - 1 : in_y);
            uint32_t p_out = src[(ptrdiff_t)sy_out * w + x];
            uint32_t p_in = src[(ptrdiff_t)sy_in * w + x];
            aa += ((p_in >> 24) & 0xFF) - ((p_out >> 24) & 0xFF);
            ra += ((p_in >> 16) & 0xFF) - ((p_out >> 16) & 0xFF);
            ga += ((p_in >> 8) & 0xFF) - ((p_out >> 8) & 0xFF);
            ba += (p_in & 0xFF) - (p_out & 0xFF);
        }
    }
}

/*
 * One box-blur pass: horizontal sliding window (src → scratch), then
 * vertical sliding window (scratch → dst).  Border pixels are clamped
 * (replicated edge).  All four ARGB channels are blurred independently.
 * Runs in O(W×H) time, independent of the box radius @r.
 *
 * This is the scalar-only path; SSE2 callers should use the split
 * hpass/vpass functions directly.
 */
static void box_blur_pass(uint32_t *src, uint32_t *dst, uint32_t *scratch,
                            int w, int h, int r) {
    box_blur_hpass_scalar(src, scratch, w, h, r);
    box_blur_vpass(scratch, dst, w, h, r);
}

/*
 * Pure blur computation: three box-blur ping-pong passes on a raw
 * pixel buffer.  Caller is responsible for flush/mark_dirty.
 */
static void do_blur(uint32_t *data, int w, int h, int sigma) {
    uint32_t *buf1 = malloc((size_t)w * h * sizeof(uint32_t));
    uint32_t *scratch = malloc((size_t)w * h * sizeof(uint32_t));
    if (!buf1 || !scratch) {
        free(buf1);
        free(scratch);
        return;
    }

    /* Compute three box radii approximating Gaussian(sigma) */
    int boxes[3];
    boxes_for_gauss(sigma, boxes);

    /*
     * Choose SSE2 or scalar horizontal pass at runtime.
     * SSE2 is baseline on x86_64; on other archs we fall back to scalar.
     */
#ifdef __x86_64__
    /* SSE2 is baseline on x86_64, always available */
    static bool use_sse2 = true;
#else
    static bool use_sse2 = false;
#endif

    /*
     * Three ping-pong passes between data (buf0) and buf1:
     *   pass 0: data → buf1  (via scratch)
     *   pass 1: buf1 → data  (via scratch)
     *   pass 2: data → buf1  (via scratch)
     * Final result lands in buf1; copy back to data.
     */
    if (use_sse2) {
#ifdef __x86_64__
        box_blur_hpass_sse2(data, scratch, w, h, boxes[0]);
        box_blur_vpass(scratch, buf1, w, h, boxes[0]);
        box_blur_hpass_sse2(buf1, scratch, w, h, boxes[1]);
        box_blur_vpass(scratch, data, w, h, boxes[1]);
        box_blur_hpass_sse2(data, scratch, w, h, boxes[2]);
        box_blur_vpass(scratch, buf1, w, h, boxes[2]);
#endif
    } else {
        box_blur_pass(data, buf1, scratch, w, h, boxes[0]);
        box_blur_pass(buf1, data, scratch, w, h, boxes[1]);
        box_blur_pass(data, buf1, scratch, w, h, boxes[2]);
    }
    memcpy(data, buf1, (size_t)w * h * sizeof(uint32_t));

    free(buf1);
    free(scratch);
}

/*
 * Applies a fast approximate Gaussian blur of standard deviation @sigma
 * to @surface in-place.
 *
 * Uses three sequential box-blur passes (Kovesi 2010), which runs in
 * O(W×H) time regardless of @sigma — replacing the original O(σ²)
 * repeated-box-filter algorithm that was catastrophically slow at high
 * sigma values on 4K displays.
 *
 * When @scale > 1, blurs at reduced resolution (w/scale × h/scale)
 * then upscales back — trading a small amount of quality for a
 * significant speed-up on large displays.
 */
void blur_image_surface(cairo_surface_t *surface, int sigma, int scale) {
    if (cairo_surface_status(surface))
        return;

    if (sigma <= 0)
        return;

    cairo_format_t fmt = cairo_image_surface_get_format(surface);
    if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24)
        return;

    cairo_surface_flush(surface);

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);

    if (scale <= 1) {
        uint32_t *data = (uint32_t *)cairo_image_surface_get_data(surface);
        do_blur(data, w, h, sigma);
        cairo_surface_mark_dirty(surface);
        return;
    }

    /* scale > 1: downscale, blur the small surface, upscale back */
    int small_w = w / scale;
    int small_h = h / scale;
    if (small_w < 1) small_w = 1;
    if (small_h < 1) small_h = 1;

    cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, small_w, small_h);
    if (cairo_surface_status(small) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(small);
        /* Fallback: blur at full resolution */
        uint32_t *data = (uint32_t *)cairo_image_surface_get_data(surface);
        do_blur(data, w, h, sigma);
        cairo_surface_mark_dirty(surface);
        return;
    }

    /* Downscale: paint original onto small surface with scaling */
    cairo_t *ctx = cairo_create(small);
    cairo_scale(ctx, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(ctx, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_GOOD);
    cairo_paint(ctx);
    cairo_destroy(ctx);

    /* Blur the small surface */
    cairo_surface_flush(small);
    uint32_t *small_data = (uint32_t *)cairo_image_surface_get_data(small);
    int blur_sigma = sigma / scale;
    if (blur_sigma < 1) blur_sigma = 1;
    do_blur(small_data, small_w, small_h, blur_sigma);
    cairo_surface_mark_dirty(small);

    /* Upscale back: paint small surface onto original */
    ctx = cairo_create(surface);
    cairo_scale(ctx, scale, scale);
    cairo_set_source_surface(ctx, small, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_GOOD);
    cairo_paint(ctx);
    cairo_destroy(ctx);

    cairo_surface_destroy(small);
    cairo_surface_mark_dirty(surface);
}