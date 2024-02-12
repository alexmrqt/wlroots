#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdlib.h>
#include <drm_fourcc.h>

#include <wlr/util/log.h>

#include <libdrm/exynos_drmif.h>
#include <exynos/exynos_drm.h>
#include <exynos/exynos_fimg2d.h>

#include "render/g2d.h"

static const struct wlr_renderer_impl renderer_impl;

bool wlr_renderer_is_g2d(struct wlr_renderer *wlr_renderer) {
	return wlr_renderer->impl == &renderer_impl;
}

static struct wlr_g2d_renderer *g2d_get_renderer(struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer_is_g2d(wlr_renderer));
	struct wlr_g2d_renderer *renderer = wl_container_of(wlr_renderer, renderer, wlr_renderer);
	return renderer;
}

struct wlr_renderer *wlr_g2d_renderer_create_with_drm_fd(int drm_fd) {
	wlr_log(WLR_INFO, "The G2D renderer is only experimental and "
		"not expected to be ready for daily use");

	struct wlr_g2d_renderer *renderer = calloc(1, sizeof(*renderer));
	if (renderer == NULL) {
		return NULL;
	}
	wlr_renderer_init(&renderer->wlr_renderer, &renderer_impl);
	wl_list_init(&renderer->textures);

	renderer->dev = exynos_device_create(drm_fd);
	if (!renderer->dev) {
		wlr_log(WLR_ERROR, "Failed to create exynos device");
		return NULL;
	}

	renderer->ctx = g2d_init(drm_fd);
	if (!renderer->ctx) {
		wlr_log(WLR_ERROR, "Failed to initialize G2D");
		return NULL;
	}

	renderer->drm_fd = drm_fd;

	size_t len = 0;
	const uint32_t *formats = get_g2d_drm_formats(&len);

	for (size_t i = 0; i < len; ++i) {
		wlr_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_INVALID);
		wlr_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_LINEAR);
		wlr_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_SAMSUNG_64_32_TILE);
		wlr_drm_format_set_add(&renderer->drm_formats, formats[i],
			DRM_FORMAT_MOD_SAMSUNG_16_16_TILE);
	}

	return &renderer->wlr_renderer;
}

static bool g2d_image_from_wlr_buffer(struct wlr_renderer *wlr_renderer,
		struct g2d_image *img, struct wlr_buffer *buffer, uint32_t flags) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_dmabuf_attributes dmabuf;
	void *data;
	uint32_t format;
	size_t stride;

	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "Cannot create g2d_image from NULL wlr_buffer");
		return false;
	}

	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		exynos_prime_fd_to_handle(renderer->dev, dmabuf.fd[0], &img->bo[0]);
		img->stride = dmabuf.stride[0];
		img->buf_type = G2D_IMGBUF_GEM;
		img->color_mode = get_g2d_format_from_drm(dmabuf.format);
	} else if (wlr_buffer_begin_data_ptr_access(buffer, flags, &data, &format,
				&stride)) {
		wlr_buffer_end_data_ptr_access(buffer);

		img->user_ptr[0].userptr = (unsigned int)data;
		img->user_ptr[0].size = buffer->height * stride;
		img->stride = stride;
		img->buf_type = G2D_IMGBUF_USERPTR;
		img->color_mode = get_g2d_format_from_drm(format);
	} else {
		wlr_log(WLR_ERROR, "Cannot create g2d_image from a buffer that is not dma-buf nor readable");
		return false;
	}

	img->width = buffer->width;
	img->height = buffer->height;

	return true;
}

static struct wlr_buffer* wlr_g2d_gem_buffer_from_wlr_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer_src) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_gem_buffer *buffer_gem;
	struct wlr_buffer *buffer_dst;
	struct g2d_image img_src = {0};
	struct g2d_image img_dst = {0};
	void *data_src;
	uint32_t format_src;
	size_t stride_src;
	int err;

	if (buffer_src == NULL) {
		wlr_log(WLR_ERROR, "Cannot copy NULL wlr_buffer to a wlr_g2d_gem_buffer");
		return false;
	}

	if (wlr_buffer_begin_data_ptr_access(buffer_src,
				WLR_BUFFER_DATA_PTR_ACCESS_READ, &data_src,
				&format_src, &stride_src)) {
		wlr_buffer_end_data_ptr_access(buffer_src);

		buffer_gem = create_g2d_gem_buffer(renderer->dev,
				buffer_src->width, buffer_src->height,
				wlr_drm_format_set_get(&renderer->drm_formats, format_src));
		if (!buffer_gem) {
			wlr_log(WLR_ERROR, "Failed to create a new wlr_g2d_gem_buffer");
			return NULL;
		}

		buffer_dst = &buffer_gem->base;
		if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_src, buffer_src,
					WLR_BUFFER_DATA_PTR_ACCESS_READ))
			return NULL;
		if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_dst, buffer_dst,
					WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
			return NULL;

		err = g2d_copy(renderer->ctx, &img_src, &img_dst,
				0, 0, 0, 0, buffer_src->width, buffer_src->height);
		if (err < 0) {
			wlr_log(WLR_ERROR, "Error when invoking g2d_copy (%d)", err);
			return NULL;
		}

		err = g2d_exec(renderer->ctx);
		if (err < 0) {
			wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", err);
			return NULL;
		}

		return buffer_dst;
	} else {
		wlr_log(WLR_ERROR, "Cannot copy non-readable wlr_buffer to wlr_g2d_gem_buffer");
		return NULL;
	}
}

static const struct wlr_texture_impl texture_impl;

bool wlr_texture_is_g2d(struct wlr_texture *texture) {
	return texture->impl == &texture_impl;
}

static struct wlr_g2d_texture *g2d_get_texture(
		struct wlr_texture *wlr_texture) {
	assert(wlr_texture_is_g2d(wlr_texture));
	struct wlr_g2d_texture *texture = wl_container_of(wlr_texture, texture, wlr_texture);
	return texture;
}

static bool g2d_texture_update_from_buffer(struct wlr_texture *texture,
		struct wlr_buffer *buffer, pixman_region32_t *damage) {
	struct wlr_renderer *wlr_renderer = g2d_get_texture(texture)->renderer;
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_buffer *buffer_src = buffer;
	struct wlr_buffer *buffer_dst = g2d_get_texture(texture)->buffer;
	struct g2d_image img_src = {0};
	struct g2d_image img_dst = {0};
	int rects_len = 0;
	int err = 0;

	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_src, buffer_src, WLR_BUFFER_DATA_PTR_ACCESS_READ))
		return false;
	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_dst, buffer_dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
		return false;

	pixman_box32_t *rects = pixman_region32_rectangles(damage, &rects_len);
	for (int i = 0; i < rects_len; i++) {
		pixman_box32_t rect = rects[i];
		int width = rect.x2 - rect.x1;
		int height = rect.y2 - rect.y1;

		err = g2d_copy(renderer->ctx, &img_src, &img_dst,
				rect.x1, rect.y1, rect.x1, rect.y1, width, height);
		if (err < 0)
			wlr_log(WLR_ERROR, "Error when invoking g2d_copy (%d)", err);
	}

	err = g2d_exec(renderer->ctx);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", err);
		return false;
	}

	return true;
}

static void g2d_texture_destroy(struct wlr_texture *wlr_texture) {
	struct wlr_g2d_texture *texture = g2d_get_texture(wlr_texture);

	wlr_buffer_unlock(texture->buffer);
	if (wlr_buffer_is_g2d_gem(texture->buffer))
		wlr_buffer_drop(texture->buffer);

	wl_list_remove(&texture->link);
	free(texture);
}

static const struct wlr_texture_impl texture_impl = {
	.update_from_buffer = g2d_texture_update_from_buffer,
	.destroy = g2d_texture_destroy,
};

static bool g2d_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_dmabuf_attributes dmabuf = {0};

	if (renderer->current_buffer != NULL) {
		wlr_buffer_unlock(renderer->current_buffer);
		renderer->current_buffer = NULL;
	}

	if (wlr_buffer == NULL) {
		return true;
	}

	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		wlr_log(WLR_ERROR, "Buffer is not a dma-buf");
		return false;
	}

	if (dmabuf.n_planes > 1)
		wlr_log(WLR_INFO, "DMA-BUF has %d planes, but G2D renderer "
				"only supports a single one", dmabuf.n_planes);

	renderer->current_buffer = wlr_buffer_lock(wlr_buffer);

	return true;
}

static void g2d_begin(struct wlr_renderer *wlr_renderer, uint32_t width,
		uint32_t height) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);

	renderer->width = width;
	renderer->height = height;

	renderer->scissor_box.x = 0;
	renderer->scissor_box.y = 0;
	renderer->scissor_box.width = width;
	renderer->scissor_box.height = height;

	assert(renderer->current_buffer != NULL);
}

static void g2d_end(struct wlr_renderer *wlr_renderer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);

	assert(renderer->current_buffer != NULL);
}

static unsigned int float_to_g2d_color(const float color[static 4], uint32_t g2d_format) {
	float red = color[0];
	float green = color[1];
	float blue = color[2];
	float alpha = color[3];

	switch (g2d_format) {
		case G2D_COLOR_FMT_XRGB8888|G2D_ORDER_AXRGB:
			return    (int)(alpha*0xFF)<<24 \
				| (int)(red*0xFF)<<16 \
				| (int)(green*0xFF)<<8 \
				| (int)(blue*0xFF);
		case G2D_COLOR_FMT_XRGB8888|G2D_ORDER_RGBAX:
			return    (int)(red*0xFF)<<24 \
				| (int)(green*0xFF)<<16 \
				| (int)(blue*0xFF)<<8 \
				| (int)(alpha*0xFF);
		case G2D_COLOR_FMT_XRGB8888|G2D_ORDER_AXBGR:
			return    (int)(alpha*0xFF)<<24 \
				| (int)(blue*0xFF)<<16 \
				| (int)(green*0xFF)<<8 \
				| (int)(red*0xFF);
		case G2D_COLOR_FMT_XRGB8888|G2D_ORDER_BGRAX:
			return    (int)(blue*0xFF)<<24 \
				| (int)(green*0xFF)<<16 \
				| (int)(red*0xFF)<<8 \
				| (int)(alpha*0xFF);
		case G2D_COLOR_FMT_ARGB8888|G2D_ORDER_AXRGB:
			return    (int)(alpha*0xFF)<<24 \
				| (int)(red*0xFF)<<16 \
				| (int)(green*0xFF)<<8 \
				| (int)(blue*0xFF);
		case G2D_COLOR_FMT_RGB565|G2D_ORDER_AXRGB:
			return    (int)(red*0x1F)<<11 \
				| (int)(green*0x3F)<<5 \
				| (int)(blue*0x1F);
		case G2D_COLOR_FMT_XRGB1555|G2D_ORDER_AXRGB:
			return    (int)(alpha*0x01)<<15 \
				| (int)(red*0x1F)<<10 \
				| (int)(green*0x1F)<<5 \
				| (int)(blue*0x1F);
		case G2D_COLOR_FMT_ARGB1555|G2D_ORDER_AXRGB:
			return    (int)(alpha*0x01)<<15 \
				| (int)(red*0x1F)<<10 \
				| (int)(green*0x1F)<<5 \
				| (int)(blue*0x1F);
		case G2D_COLOR_FMT_XRGB4444|G2D_ORDER_AXRGB:
			return    (int)(alpha*0xF)<<12 \
				| (int)(red*0xF)<<8 \
				| (int)(green*0xF)<<4 \
				| (int)(blue*0xF);
		case G2D_COLOR_FMT_ARGB4444|G2D_ORDER_AXRGB:
			return    (int)(alpha*0xF)<<12 \
				| (int)(red*0xF)<<8 \
				| (int)(green*0xF)<<4 \
				| (int)(blue*0xF);
		default:
			return 0;
	}
}

static void g2d_clear(struct wlr_renderer *wlr_renderer,
		const float color[static 4]) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_buffer *buffer = renderer->current_buffer;
	struct g2d_image img = {0};
	int ret = 0;

	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img, buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
		return;
	img.color = float_to_g2d_color(color, img.color_mode);

	ret = g2d_solid_fill(renderer->ctx, &img,
			renderer->scissor_box.x,
			renderer->scissor_box.y,
			renderer->scissor_box.width,
			renderer->scissor_box.height);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_solid_fill (%d)", ret);
		return;
	}

	ret = g2d_exec(renderer->ctx);
	if (ret < 0)
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", ret);
}

static void g2d_scissors(struct wlr_renderer *wlr_renderer,
		struct wlr_box *box) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);

	uint32_t w = renderer->width;
	uint32_t h = renderer->height;
	struct wlr_box dst = {0, 0, w, h};
	if (box && !wlr_box_intersection(&dst, box, &dst)) {
		dst = (struct wlr_box) {0, 0, 0, 0}; // empty
	}

	renderer->scissor_box = dst;
}

static bool can_g2d_handle_transform(const float matrix[static 9]) {
	return matrix[1] < 1e-5f && matrix[1] > -1e-5f && matrix[3] < 1e-5f && matrix[3] > -1e-5f && matrix[0] > 0.0 && matrix[4] > 0.0;
}

static bool g2d_render_subtexture_with_matrix(
		struct wlr_renderer *wlr_renderer, struct wlr_texture *wlr_texture,
		const struct wlr_fbox *fbox, const float matrix[static 9],
		float alpha) {
	if (!can_g2d_handle_transform(matrix)) {
		wlr_log(WLR_ERROR, "G2D renderer can only handle translation and scaling");
		return false;
	}

	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_texture *texture = g2d_get_texture(wlr_texture);
	struct wlr_buffer *buffer_src = texture->buffer;
	struct wlr_buffer *buffer_dst = renderer->current_buffer;
	struct wlr_box box_src, box_dst;

	struct g2d_image img_src = {0};
	struct g2d_image img_dst = {0};

	float alpha_x, alpha_y, beta_x, beta_y;
	int err = 0;

	box_dst.x = roundf(fbox->x*matrix[0] + matrix[2]);
	box_dst.y = roundf(fbox->y*matrix[4] + matrix[5]);
	box_dst.width = roundf(fbox->width*matrix[0]);
	box_dst.height = roundf(fbox->height*matrix[4]);

	// Apply scissors
	if (!wlr_box_intersection(&box_dst, &renderer->scissor_box, &box_dst))
		return true;

	// Compute box_dst scaled and translated coordinates in texture
	alpha_x = texture->wlr_texture.width/fbox->width;
	beta_x = alpha_x*fbox->x*matrix[0]/fbox->width + matrix[2];
	alpha_y = texture->wlr_texture.height/fbox->height;
	beta_y = alpha_y*fbox->y*matrix[4]/fbox->height + matrix[5];

	box_src.x = box_dst.x*alpha_x - beta_x;
	box_src.y = box_dst.y*alpha_y - beta_y;
	box_src.width = roundf(box_dst.width * alpha_x);
	box_src.height = roundf(box_dst.height * alpha_y);

	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_src, buffer_src,
				WLR_BUFFER_DATA_PTR_ACCESS_READ))
		return false;
	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_dst, buffer_dst,
				WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
		return false;

	// Copy in renderer buffer
	err = g2d_copy_with_scale(renderer->ctx, &img_src, &img_dst,
			box_src.x, box_src.y, box_src.width, box_src.height,
			box_dst.x, box_dst.y, box_dst.width, box_dst.height, 0);
			
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_scale_and_blend (%d)", err);
		return false;
	}

	err = g2d_exec(renderer->ctx);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", err);
		return false;
	}

	return true;
}

static void g2d_render_quad_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	if (!can_g2d_handle_transform(matrix)) {
		wlr_log(WLR_ERROR, "G2D renderer can only handle translation and scaling");
		return;
	}

	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_buffer *buffer = renderer->current_buffer;
	struct g2d_image img = {0};
	struct wlr_box box = {0, 0, 0, 0};
	int ret = 0;

	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img, buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE))
		return;

	// Convert matrix to int
	int m[9];
	for (int i=0 ; i < 9 ; ++i)
		m[i] = (int)roundf(matrix[i]);

	// Compute box coordinates
	box.x = m[2];
	box.y = m[5];
	box.width = m[0];
	box.height = m[4];

	// Apply scissors
	if (!wlr_box_intersection(&box, &renderer->scissor_box, &box)) {
		return;
	}

	img.color = float_to_g2d_color(color, img.color_mode);
	ret = g2d_solid_fill(renderer->ctx, &img, 
			box.x, box.y, box.width, box.height);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_blend (%d)", ret);
		return;
	}

	ret = g2d_exec(renderer->ctx);
	if (ret < 0)
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", ret);
}

static const uint32_t *g2d_get_shm_texture_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	return get_g2d_drm_formats(len);
}

static const struct wlr_drm_format_set *g2d_get_render_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	return &renderer->drm_formats;
}

static uint32_t g2d_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_buffer *buffer = renderer->current_buffer;
	struct wlr_dmabuf_attributes dmabuf;
	uint32_t format = DRM_FORMAT_INVALID;

	if (wlr_buffer_get_dmabuf(buffer, &dmabuf))
		format = dmabuf.format;
	else if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, NULL, &format, NULL))
		wlr_buffer_end_data_ptr_access(buffer);

	return format;
}

static bool g2d_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_buffer *buffer_src = renderer->current_buffer;
	struct g2d_image img_src = {0};
	struct g2d_image img_dst = {0};
	int err = 0;

	if (!g2d_image_from_wlr_buffer(wlr_renderer, &img_src, buffer_src, WLR_BUFFER_DATA_PTR_ACCESS_READ))
		return false;

	img_dst.width = width;
	img_dst.height = height;
	img_dst.user_ptr[0].userptr = (unsigned int)data;
	img_dst.user_ptr[0].size = height * stride;
	img_dst.stride = stride;
	img_dst.buf_type = G2D_IMGBUF_USERPTR;
	img_dst.color_mode = get_g2d_format_from_drm(drm_format);

	err = g2d_copy(renderer->ctx, &img_src, &img_dst,
			src_x, src_y, dst_x, dst_y, width, height);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_copy (%d)", err);
		return false;
	}

	err = g2d_exec(renderer->ctx);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", err);
		return false;
	}

	return true;
}

static void g2d_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_texture *tex, *tex_tmp;

	wl_list_for_each_safe(tex, tex_tmp, &renderer->textures, link) {
		wlr_texture_destroy(&tex->wlr_texture);
	}

	wlr_drm_format_set_finish(&renderer->drm_formats);

	g2d_fini(renderer->ctx);
	exynos_device_destroy(renderer->dev);

	free(renderer);
}

static uint32_t g2d_get_render_buffer_caps(struct wlr_renderer *wlr_renderer) {
	return WLR_BUFFER_CAP_DMABUF;
}

static struct wlr_texture *g2d_texture_from_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_texture *texture;
	struct wlr_dmabuf_attributes dmabuf;

	wl_list_for_each(texture, &renderer->textures, link) {
		if (texture->buffer == buffer) {
			return &texture->wlr_texture;
		}
	}

	texture = calloc(1, sizeof(*texture));
	if (texture == NULL)
		goto err_out;

	wlr_texture_init(&texture->wlr_texture, &texture_impl, buffer->width, buffer->height);
	texture->renderer = wlr_renderer;

	if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		texture->buffer = wlr_g2d_gem_buffer_from_wlr_buffer(wlr_renderer, buffer);
		if (!texture->buffer)
			goto err_free_texture;
		texture->buffer = wlr_buffer_lock(texture->buffer);
	}
	else
		texture->buffer = wlr_buffer_lock(buffer);

	wl_list_insert(&renderer->textures, &texture->link);

	return &texture->wlr_texture;

err_free_texture:
	free(texture);

err_out:
	wlr_log(WLR_ERROR, "Failed to allocate g2d texture");
	return NULL;
}

static const struct wlr_renderer_impl renderer_impl = {
	.bind_buffer = g2d_bind_buffer,
	.begin = g2d_begin,
	.end = g2d_end,
	.clear = g2d_clear,
	.scissor = g2d_scissors,
	.render_subtexture_with_matrix = g2d_render_subtexture_with_matrix,
	.render_quad_with_matrix = g2d_render_quad_with_matrix,
	.get_shm_texture_formats = g2d_get_shm_texture_formats,
	.get_render_formats = g2d_get_render_formats,
	.preferred_read_format = g2d_preferred_read_format,
	.read_pixels = g2d_read_pixels,
	.destroy = g2d_destroy,
	.get_render_buffer_caps = g2d_get_render_buffer_caps,
	.texture_from_buffer = g2d_texture_from_buffer,
};