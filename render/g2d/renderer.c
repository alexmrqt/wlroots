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
	wl_list_init(&renderer->buffers);
	wl_list_init(&renderer->textures);

	renderer->ctx = g2d_init(drm_fd);
	if (!renderer->ctx) {
		wlr_log(WLR_ERROR, "Failed to initialize G2D");
		return NULL;
	}

	renderer->dev = exynos_device_create(drm_fd);
	if (!renderer->dev) {
		wlr_log(WLR_ERROR, "Failed to create exynos device");
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

static const struct wlr_texture_impl texture_impl;

bool wlr_texture_is_g2d(struct wlr_texture *texture) {
	return texture->impl == &texture_impl;
}

static struct wlr_g2d_texture *get_texture(
		struct wlr_texture *wlr_texture) {
	assert(wlr_texture_is_g2d(wlr_texture));
	struct wlr_g2d_texture *texture = wl_container_of(wlr_texture, texture, wlr_texture);
	return texture;
}

static void texture_destroy(struct wlr_texture *wlr_texture) {
	struct wlr_g2d_texture *texture = get_texture(wlr_texture);
	wl_list_remove(&texture->link);
	exynos_bo_destroy(texture->bo);
	free(texture);
}

static const struct wlr_texture_impl texture_impl = {
	.destroy = texture_destroy,
};

static void destroy_buffer(struct wlr_g2d_buffer *buffer) {
	int drm_fd = buffer->renderer->drm_fd;

	wl_list_remove(&buffer->link);
	wl_list_remove(&buffer->buffer_destroy.link);

	for (int i=0 ; i < G2D_PLANE_MAX_NR ; ++i) {
		if (buffer->image.bo[i]) {
			drmCloseBufferHandle(drm_fd, buffer->image.bo[i]);
		}
	}

	free(buffer);
}

static void handle_destroy_buffer(struct wl_listener *listener, void *data) {
	struct wlr_g2d_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	destroy_buffer(buffer);
}

static struct wlr_g2d_buffer *get_or_create_buffer(struct wlr_g2d_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_g2d_buffer *buffer;
	struct wlr_dmabuf_attributes dmabuf = {0};
	bool buf_found = false;

	wl_list_for_each(buffer, &renderer->buffers, link) {
		if (buffer->buffer == wlr_buffer) {
			buf_found = true;
			break;
		}
	}

	if (!buf_found) {
		buffer = calloc(1, sizeof(*buffer));
		if (buffer == NULL) {
			wlr_log_errno(WLR_ERROR, "Allocation failed");
			return NULL;
		}
		buffer->buffer = wlr_buffer;
		buffer->renderer = renderer;

		buffer->buffer_destroy.notify = handle_destroy_buffer;
		wl_signal_add(&wlr_buffer->events.destroy, &buffer->buffer_destroy);

		wl_list_insert(&renderer->buffers, &buffer->link);

		wlr_log(WLR_DEBUG, "Creating G2D GEM buffer %dx%d",
			wlr_buffer->width, wlr_buffer->height);
	}

	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		wlr_log(WLR_ERROR, "Buffer is not a dma-buf");
		goto error_buffer;
	}

	assert(dmabuf.n_planes < G2D_PLANE_MAX_NR);

	for (int i=0 ; i < dmabuf.n_planes ; ++i) {
		exynos_prime_fd_to_handle(renderer->dev, dmabuf.fd[i], &buffer->image.bo[i]);
	}
	buffer->image.width = dmabuf.width;
	buffer->image.height = dmabuf.height;
	//G2D does not support multiple strides in a single g2d_image
	buffer->image.stride = dmabuf.stride[0];
	buffer->image.buf_type = G2D_IMGBUF_GEM;
	buffer->image.color_mode = get_g2d_format_from_drm(dmabuf.format);

	return buffer;

error_buffer:
	free(buffer);
	return NULL;
}

static bool g2d_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);

	if (renderer->current_buffer != NULL) {
		wlr_buffer_unlock(renderer->current_buffer->buffer);
		renderer->current_buffer = NULL;
	}

	if (wlr_buffer == NULL) {
		return true;
	}

	struct wlr_g2d_buffer *buffer = get_or_create_buffer(renderer, wlr_buffer);
	if (buffer == NULL) {
		wlr_log(WLR_ERROR, "Failed to create and bind a new buffer");
		return false;
	}

	wlr_buffer_lock(wlr_buffer);
	renderer->current_buffer = buffer;

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

unsigned int float_to_g2d_color(const float color[static 4], uint32_t g2d_format) {
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
	struct wlr_g2d_buffer *buffer = renderer->current_buffer;
	int ret = 0;

	buffer->image.color = float_to_g2d_color(color, buffer->image.color_mode);

	ret = g2d_solid_fill(renderer->ctx, &buffer->image,
			renderer->scissor_box.x,
			renderer->scissor_box.y,
			renderer->scissor_box.width,
			renderer->scissor_box.height);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_solid_fill (%d)", ret);
	}

	ret = g2d_exec(renderer->ctx);
	if (ret < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec (%d)", ret);
	}
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

bool can_g2d_handle_transform(const float matrix[static 9]) {
	return matrix[1] < 1e-5f || matrix[1] > -1e-5f || matrix[3] < 1e-5f || matrix[3] > -1e-5f || matrix[0] < 0.0 || matrix[4] < 0.0;
}

//TODO: support alpha
static bool g2d_render_subtexture_with_matrix(
		struct wlr_renderer *wlr_renderer, struct wlr_texture *wlr_texture,
		const struct wlr_fbox *fbox, const float matrix[static 9],
		float alpha) {
	// Only simple translation and scaling can be accelerated
	if (!can_g2d_handle_transform(matrix)) {
		wlr_log(WLR_ERROR, "G2D cannot render complex matrix transformations");
		return false;
	}

	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_texture *texture = get_texture(wlr_texture);
	struct wlr_g2d_buffer *buffer = renderer->current_buffer;
	struct wlr_box box_src, box_dst;

	float alpha_x, alpha_y, beta_x, beta_y;
	int err = 0;

	box_dst.x = roundf(fbox->x*matrix[0]/fbox->width + matrix[2]);
	box_dst.y = roundf(fbox->y*matrix[4]/fbox->height + matrix[5]);
	box_dst.width = roundf(fbox->width);
	box_dst.height = roundf(fbox->height);

	// Apply scissors
	if (!wlr_box_intersection(&box_dst, &renderer->scissor_box, &box_dst))
		return true;

	// Compute box_dst scaled and translated coordinates in texture
	alpha_x = texture->image.width/fbox->width;
	beta_x = alpha_x*fbox->x*matrix[0]/fbox->width + matrix[2];
	alpha_y = texture->image.height/fbox->height;
	beta_y = alpha_y*fbox->y*matrix[4]/fbox->height + matrix[5];

	box_src.x = box_dst.x*alpha_x - beta_x;
	box_src.y = box_dst.y*alpha_y - beta_y;
	box_src.width = roundf(box_dst.width * alpha_x);
	box_src.height = roundf(box_dst.height * alpha_y);

	// Blend with renderer buffer
	err = g2d_scale_and_blend(renderer->ctx, &texture->image, &buffer->image,
			box_src.x, box_src.y, box_src.width, box_src.height,
			box_dst.x, box_dst.y, box_dst.width, box_dst.height, G2D_OP_OVER);
			
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
	// Only simple translation and scaling can be accelerated
	if (!can_g2d_handle_transform(matrix)) {
		wlr_log(WLR_ERROR, "G2D cannot render complex matrix transformations");
		return;
	}

	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_buffer *buffer = renderer->current_buffer;
	struct wlr_box box = {0, 0, 0, 0};
	int ret = 0;

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

	buffer->image.color = float_to_g2d_color(color, buffer->image.color_mode);
	ret = g2d_solid_fill(renderer->ctx, &buffer->image, 
			box.x, box.y, box.width, box.height);
	if (ret < 0)
		wlr_log(WLR_ERROR, "Error when invoking g2d_blend (%d)", ret);

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
	return get_drm_format_from_g2d(renderer->current_buffer->image.color_mode);
}

static bool g2d_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_buffer *buffer = renderer->current_buffer;
	struct g2d_image src_img = {0};
	struct wlr_box box_src, box_dst;
	int err = 0;

	box_dst.x = dst_x;
	box_dst.y = dst_y;
	box_dst.width = width;
	box_dst.height = height;

	// Apply scissors
	if (!wlr_box_intersection(&box_dst, &renderer->scissor_box, &box_dst))
		return true;

	box_src.x = src_x + box_dst.x - dst_x;
	box_src.y = src_y + box_dst.y - dst_y;
	box_src.width = box_dst.width;
	box_src.height = box_dst.height;

	src_img.width = width;
	src_img.height = height;
	src_img.stride = stride;
	src_img.buf_type = G2D_IMGBUF_USERPTR;
	src_img.color_mode = get_g2d_format_from_drm(drm_format);
	src_img.user_ptr[0].userptr = (unsigned long)data;
	src_img.user_ptr[0].size = height * stride;

	err = g2d_blend(renderer->ctx, &src_img, &buffer->image, box_src.x, box_src.y,
			box_dst.x, box_dst.y, box_dst.width, box_dst.height, G2D_OP_SRC);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_blend");
		return false;
	}

	err = g2d_exec(renderer->ctx);
	if (err < 0) {
		wlr_log(WLR_ERROR, "Error when invoking g2d_exec");
		return false;
	}

	return true;
}

static void g2d_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	struct wlr_g2d_buffer *buffer, *buffer_tmp;
	struct wlr_g2d_texture *tex, *tex_tmp;

	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		destroy_buffer(buffer);
	}

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

static struct wlr_g2d_texture *g2d_texture_create(
		struct wlr_g2d_renderer *renderer,
		uint32_t width, uint32_t height) {
	struct wlr_g2d_texture *texture = calloc(1, sizeof(*texture));
	if (texture == NULL) {
		wlr_log_errno(WLR_ERROR, "Failed to allocate g2d texture");
		return NULL;
	}

	wlr_texture_init(&texture->wlr_texture, &texture_impl, width, height);

	texture->renderer = renderer;

	wl_list_insert(&renderer->textures, &texture->link);

	return texture;
}

static struct wlr_texture *g2d_texture_from_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *buffer) {
	struct wlr_g2d_renderer *renderer = g2d_get_renderer(wlr_renderer);
	void *data, *mapped_ptr;
	uint32_t format;
	size_t stride;
	struct wlr_dmabuf_attributes dmabuf;

	struct wlr_g2d_texture *texture = g2d_texture_create(renderer, buffer->width, buffer->height);

	if (wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
		assert(dmabuf.n_planes < G2D_PLANE_MAX_NR);

		for (int i=0 ; i < dmabuf.n_planes ; ++i) {
			exynos_prime_fd_to_handle(renderer->dev, dmabuf.fd[i],
					&texture->image.bo[i]);
		}
		texture->image.width = dmabuf.width;
		texture->image.height = dmabuf.height;
		//G2D does not support multiple strides in a single g2d_image
		texture->image.stride = dmabuf.stride[0];
		texture->image.buf_type = G2D_IMGBUF_GEM;
		texture->image.color_mode = get_g2d_format_from_drm(dmabuf.format);
	} else if (wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		texture->bo = exynos_bo_create(renderer->dev, buffer->height * stride, 0);
		if (!texture->bo)
			return NULL;
		mapped_ptr = exynos_bo_map(texture->bo);
		if (!mapped_ptr)
			return NULL;

		memcpy(mapped_ptr, data, buffer->height * stride);

		texture->image.bo[0] = texture->bo->handle;
		texture->image.width = buffer->width;
		texture->image.height = buffer->height;
		texture->image.stride = stride;
		texture->image.buf_type = G2D_IMGBUF_GEM;
		texture->image.color_mode = get_g2d_format_from_drm(format);

		wlr_buffer_end_data_ptr_access(buffer);
	} else {
		return NULL;
	}

	return &texture->wlr_texture;
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
