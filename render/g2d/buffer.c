#define _POSIX_C_SOURCE 200809L


#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>

#include "render/g2d.h"
#include "render/drm_format_set.h"

static const struct wlr_buffer_impl buffer_impl;

bool wlr_buffer_is_g2d_gem(struct wlr_buffer *wlr_buffer) {
	return wlr_buffer->impl == &buffer_impl;
}

static struct wlr_g2d_gem_buffer *g2d_buffer_from_buffer(
		struct wlr_buffer *wlr_buf) {
	assert(wlr_buf->impl == &buffer_impl);
	return (struct wlr_g2d_gem_buffer *)wlr_buf;
}

struct wlr_g2d_gem_buffer *create_g2d_gem_buffer(struct exynos_device *dev,
		int width, int height, const struct wlr_drm_format *format) {
	if (!wlr_drm_format_has(format, DRM_FORMAT_MOD_INVALID) &&
			!wlr_drm_format_has(format, DRM_FORMAT_MOD_LINEAR) &&
			!wlr_drm_format_has(format, DRM_FORMAT_MOD_SAMSUNG_64_32_TILE) &&
			!wlr_drm_format_has(format, DRM_FORMAT_MOD_SAMSUNG_16_16_TILE)) {
		wlr_log(WLR_ERROR, "G2D buffers only supports INVALID, LINEAR , "
			"SAMSUNG_64_32_TILE, and SAMSUNG_16_16_TILE modifiers");
		return NULL;
	}

	const struct wlr_pixel_format_info *info =
		drm_get_pixel_format_info(format->format);
	if (info == NULL) {
		wlr_log(WLR_ERROR, "DRM format 0x%"PRIX32" not supported",
			format->format);
		return NULL;
	}
	uint32_t g2d_color_mode = get_g2d_format_from_drm(format->format);
	if (!g2d_color_mode)
		return NULL;

	struct wlr_g2d_gem_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);

	buffer->format = format->format;
	buffer->stride = (uint32_t)(width * info->bpp/8);
	buffer->size = buffer->base.height * buffer->stride;

	buffer->bo = exynos_bo_create(dev, buffer->size, 0);
	if (!buffer->bo)
		goto create_destroy;

	int prime_fd;
	exynos_prime_handle_to_fd(dev, buffer->bo->handle, &prime_fd);

	buffer->dmabuf = (struct wlr_dmabuf_attributes){
		.width = buffer->base.width,
		.height = buffer->base.height,
		.format = format->format,
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.n_planes = 1,
		.offset[0] = 0,
		.stride[0] = buffer->stride,
		.fd[0] = prime_fd,
	};

	wlr_log(WLR_DEBUG, "Allocated %"PRIu32"x%"PRIu32" exynos GEM buffer",
			buffer->base.width, buffer->base.height);

	return buffer;

create_destroy:
	wlr_buffer_drop(&buffer->base);
	return NULL;
}

static void g2d_buffer_destroy(struct wlr_buffer *buffer) {
	struct wlr_g2d_gem_buffer *buf = g2d_buffer_from_buffer(buffer);

	wlr_dmabuf_attributes_finish(&buf->dmabuf);

	exynos_bo_destroy(buf->bo);

	free(buf);
}

static bool g2d_buffer_get_dmabuf(struct wlr_buffer *buffer,
	struct wlr_dmabuf_attributes *attribs) {
	struct wlr_g2d_gem_buffer *buf = g2d_buffer_from_buffer(buffer);

	memcpy(attribs, &buf->dmabuf, sizeof(buf->dmabuf));
	return true;
}

static bool g2d_buffer_begin_data_ptr_access(struct wlr_buffer *buffer, uint32_t flags,
	void **data, uint32_t *format, size_t *stride) {
	struct wlr_g2d_gem_buffer *buf = g2d_buffer_from_buffer(buffer);
	
	if(!exynos_bo_map(buf->bo))
		return false;

	*data = buf->bo->vaddr;
	*stride = buf->stride;
	*format = buf->format;

	return true;
}

static void g2d_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = g2d_buffer_destroy,
	.get_dmabuf = g2d_buffer_get_dmabuf,
	.begin_data_ptr_access = g2d_buffer_begin_data_ptr_access,
	.end_data_ptr_access = g2d_buffer_end_data_ptr_access,
};
