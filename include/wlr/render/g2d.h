/*
 * This an unstable interface of wlroots. No guarantees are made regarding the
 * future consistency of this API.
 */
#ifndef WLR_USE_UNSTABLE
#error "Add -DWLR_USE_UNSTABLE to enable unstable wlroots features"
#endif

#ifndef WLR_RENDER_G2D_H
#define WLR_RENDER_G2D_H

#include <wlr/render/wlr_renderer.h>

#include <libdrm/exynos_drmif.h>
#include <exynos/exynos_drm.h>
#include <exynos/exynos_fimg2d.h>

struct wlr_renderer *wlr_g2d_renderer_create_with_drm_fd(int drm_fd);

bool wlr_renderer_is_g2d(struct wlr_renderer *wlr_renderer);
bool wlr_texture_is_g2d(struct wlr_texture *texture);

struct exynos_bo *exynos_mapped_buffer_create(struct exynos_device *dev, unsigned long size);
struct exynos_bo *exynos_buffer_create(struct exynos_device *dev, unsigned long size);
void exynos_buffer_destroy(struct exynos_bo *bo);

struct g2d_image g2d_image_create_bits(struct exynos_bo *bo,
		uint32_t color_mode,
		unsigned int width, unsigned int height,
		void *data, unsigned int stride);

unsigned int float_to_g2d_color(const float color[static 4], uint32_t g2d_format);
bool can_g2d_handle_transform(const float matrix[static 9]);

#endif
