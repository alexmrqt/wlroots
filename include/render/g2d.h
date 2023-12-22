#ifndef RENDER_PIXMAN_H
#define RENDER_PIXMAN_H

#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/g2d.h>
#include <wlr/render/wlr_renderer.h>

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

	struct wl_list buffers; // wlr_g2d_buffer.link
	struct wl_list textures; // wlr_g2d_texture.link

	int drm_fd;
	struct g2d_context *ctx;
	struct exynos_device *dev;

	struct wlr_g2d_buffer *current_buffer;
	uint32_t width, height;

	struct wlr_drm_format_set drm_formats;
};

struct wlr_g2d_buffer {
	struct wlr_buffer *buffer;
	struct wlr_g2d_renderer *renderer;

	struct g2d_image image;

	struct wl_listener buffer_destroy;
	struct wl_list link; // wlr_g2d_renderer.buffers
};

struct wlr_g2d_texture {
	struct wlr_texture wlr_texture;
	struct wlr_g2d_renderer *renderer;

	struct g2d_image image;
	struct exynos_bo *bo;

	struct wl_list link; // wlr_g2d_renderer.textures
};

struct wlr_g2d_render_pass {
	struct wlr_render_pass base;
	struct wlr_g2d_buffer *buffer;
};

struct wlr_gles2_renderer *gles2_get_renderer(
	struct wlr_renderer *wlr_renderer);

uint32_t get_g2d_format_from_drm(uint32_t fmt);
uint32_t get_drm_format_from_g2d(uint32_t fmt);
const uint32_t *get_g2d_drm_formats(size_t *len);

struct wlr_g2d_render_pass *begin_g2d_render_pass(
	struct wlr_g2d_buffer *buffer);

#endif
