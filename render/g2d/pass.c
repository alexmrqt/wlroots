#include <assert.h>
#include <stdlib.h>
#include <pixman.h>

#include <wlr/util/log.h>

#include <libdrm/exynos_drmif.h>

#include "render/g2d.h"

static const struct wlr_render_pass_impl render_pass_impl;

static struct wlr_g2d_render_pass *get_render_pass(struct wlr_render_pass *wlr_pass) {
	assert(wlr_pass->impl == &render_pass_impl);
	struct wlr_g2d_render_pass *pass = wl_container_of(wlr_pass, pass, base);
	return pass;
}

static struct wlr_g2d_texture *get_texture(struct wlr_texture *wlr_texture) {
	assert(wlr_texture_is_g2d(wlr_texture));
	struct wlr_g2d_texture *texture = wl_container_of(wlr_texture, texture, wlr_texture);
	return texture;
}

static bool render_pass_submit(struct wlr_render_pass *wlr_pass) {
	struct wlr_g2d_render_pass *pass = get_render_pass(wlr_pass);
	wlr_buffer_unlock(pass->buffer->buffer);

	free(pass);

	return true;
}

static enum e_g2d_op get_g2d_blending(enum wlr_render_blend_mode mode) {
	switch (mode) {
	case WLR_RENDER_BLEND_MODE_PREMULTIPLIED:
		return G2D_OP_OVER;
	case WLR_RENDER_BLEND_MODE_NONE:
		return G2D_OP_SRC;
	}
	abort();
}

//TODO: filter mode, rotations alpha
static void render_pass_add_texture(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_texture_options *options) {
	struct wlr_g2d_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_g2d_texture *texture = get_texture(options->texture);
	struct wlr_g2d_buffer *buffer = pass->buffer;
	struct wlr_fbox src_fbox;
	struct wlr_box dst_box;

	pixman_region32_t region;
	const pixman_box32_t *rects;

	struct g2d_image tex_img = texture->image;
	enum e_g2d_op op = get_g2d_blending(options->blend_mode);

	int rects_len, err;

	wlr_render_texture_options_get_src_box(options, &src_fbox);
	wlr_render_texture_options_get_dst_box(options, &dst_box);
	
	pixman_region32_init_rect(&region, dst_box.x, dst_box.y, dst_box.width, dst_box.height);
	if (options->clip)
		pixman_region32_intersect(&region, &region, options->clip);

	rects = pixman_region32_rectangles(&region, &rects_len);
	if (rects_len == 0)
		goto free_region;

	// Render each region separately
	for (int i = 0 ; i < rects_len ; ++i) {
		const pixman_box32_t *rect = &rects[i];
		int nonneg_x1 = ((rect->x1 < 0)?rect->x1:0);
		int nonneg_y1 = ((rect->y1 < 0)?rect->y1:0);
		int src_x = roundf(src_fbox.x) - nonneg_x1;
		int src_y = roundf(src_fbox.y) - nonneg_y1;
		struct wlr_box rect_box = {
			(rect->x1 < 0)?0:rect->x1,
			(rect->y1 < 0)?0:rect->y1,
			rect->x2 - nonneg_x1,
			rect->y2 - nonneg_y1};

		if ((unsigned int)src_x >= tex_img.width)
			return;
		if ((unsigned int)src_y >= tex_img.height)
			return;

		err = g2d_blend(buffer->renderer->ctx, &tex_img, &buffer->image,
				src_x, src_y,
				rect_box.x, rect_box.y,
				rect_box.width, rect_box.height, op);
		if (err < 0) {
			wlr_log(WLR_ERROR, "Error when invoking g2d_blend");
			goto free_region;
		}
	}

	err = g2d_exec(buffer->renderer->ctx);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec");
		goto free_region;
	}

free_region:
	pixman_region32_fini(&region);
}

static void render_pass_add_rect(struct wlr_render_pass *wlr_pass,
		const struct wlr_render_rect_options *options) {
	struct wlr_g2d_render_pass *pass = get_render_pass(wlr_pass);
	struct wlr_g2d_buffer *buffer = pass->buffer;
	struct wlr_box box;

	pixman_region32_t region;
	const pixman_box32_t *rects;

	//enum e_g2d_op op = get_g2d_blending(options->color.a == 1 ?
	//	WLR_RENDER_BLEND_MODE_NONE : options->blend_mode);

	int rects_len, ret;
	float color[4] = {options->color.r,
		options->color.g,
		options->color.b,
		options->color.a};
	unsigned int g2d_color = float_to_g2d_color(color, buffer->image.color_mode);

	wlr_render_rect_options_get_box(options, pass->buffer->buffer, &box);

	pixman_region32_init_rect(&region, box.x, box.y, box.width, box.height);
	if (options->clip)
		pixman_region32_intersect(&region, &region, options->clip);

	rects = pixman_region32_rectangles(&region, &rects_len);
	if (rects_len == 0)
		goto free_region;

	// Render each region separately
	for (int i = 0 ; i < rects_len ; ++i) {
		const pixman_box32_t *rect = &rects[i];
		int nonneg_x1 = ((rect->x1 < 0)?rect->x1:0);
		int nonneg_y1 = ((rect->y1 < 0)?rect->y1:0);
		struct wlr_box rect_box = {
			(rect->x1 < 0)?0:rect->x1,
			(rect->y1 < 0)?0:rect->y1,
			rect->x2 - nonneg_x1,
			rect->y2 - nonneg_y1};

		buffer->image.color = g2d_color;

		ret = g2d_solid_fill(buffer->renderer->ctx, &buffer->image,
				rect_box.x, rect_box.y,
				rect_box.width, rect_box.height);

		if (ret < 0) {
			wlr_log(WLR_ERROR, "Error when invoking g2d_solid_fill (%d)", ret);
			//goto free_region;
		}
	}

	ret = g2d_exec(buffer->renderer->ctx);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", ret);
		goto free_region;
	}

free_region:
	pixman_region32_fini(&region);
}

static const struct wlr_render_pass_impl render_pass_impl = {
	.submit = render_pass_submit,
	.add_texture = render_pass_add_texture,
	.add_rect = render_pass_add_rect,
};

struct wlr_g2d_render_pass *begin_g2d_render_pass(
		struct wlr_g2d_buffer *buffer) {
	struct wlr_g2d_render_pass *pass = calloc(1, sizeof(*pass));
	if (pass == NULL) {
		return NULL;
	}

	wlr_render_pass_init(&pass->base, &render_pass_impl);

	wlr_buffer_lock(buffer->buffer);
	pass->buffer = buffer;

	return pass;
}
