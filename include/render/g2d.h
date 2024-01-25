#ifndef RENDER_PIXMAN_H
#define RENDER_PIXMAN_H

#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/g2d.h>
#include <wlr/render/wlr_renderer.h>

#include <wlr/util/box.h>

#include <libdrm/exynos_drmif.h>
#include <exynos/exynos_drm.h>
#include <exynos/exynos_fimg2d.h>

#include "render/pixel_format.h"

struct wlr_g2d_pixel_format {
	uint32_t drm_format;
	uint32_t g2d_format;
};

struct wlr_g2d_renderer {
	struct wlr_renderer wlr_renderer;
	struct wlr_box scissor_box;

	struct wl_list textures; // wlr_g2d_buffer_list.link

	int drm_fd;
	struct g2d_context *ctx;
	struct exynos_device *dev;

	struct wlr_buffer *current_buffer;
	uint32_t width, height;

	struct wlr_drm_format_set drm_formats;
};

struct wlr_g2d_gem_buffer {
	struct wlr_buffer base;

	struct wlr_dmabuf_attributes dmabuf;

	uint32_t format;
	uint32_t stride;

	uint64_t size;
	struct exynos_bo *bo;
};

struct wlr_g2d_texture {
	struct wlr_texture wlr_texture;

	struct wlr_buffer *buffer;

	struct wl_list link; // wlr_g2d_renderer.textures
};

uint32_t get_g2d_format_from_drm(uint32_t fmt);
uint32_t get_drm_format_from_g2d(uint32_t fmt);
const uint32_t *get_g2d_drm_formats(size_t *len);

bool wlr_buffer_is_g2d_gem(struct wlr_buffer *wlr_buffer);
struct wlr_g2d_gem_buffer *create_g2d_gem_buffer(struct exynos_device *dev,
		int width, int height, const struct wlr_drm_format *format);

#endif
