#include <drm_fourcc.h>
#include <wlr/util/log.h>

#include "render/g2d.h"

static const struct wlr_g2d_pixel_format formats[] = {
	{
		.drm_format = DRM_FORMAT_XRGB4444,
		.g2d_format = G2D_COLOR_FMT_XRGB4444|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_XBGR4444,
		.g2d_format = G2D_COLOR_FMT_XRGB4444|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBX4444,
		.g2d_format = G2D_COLOR_FMT_XRGB4444|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRX4444,
		.g2d_format = G2D_COLOR_FMT_XRGB4444|G2D_ORDER_BGRAX,
	},
	{
		.drm_format = DRM_FORMAT_ARGB4444,
		.g2d_format = G2D_COLOR_FMT_ARGB4444|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_ABGR4444,
		.g2d_format = G2D_COLOR_FMT_ARGB4444|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBA4444,
		.g2d_format = G2D_COLOR_FMT_ARGB4444|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRA4444,
		.g2d_format = G2D_COLOR_FMT_ARGB4444|G2D_ORDER_BGRAX,
	},
	{
		.drm_format = DRM_FORMAT_XRGB1555,
		.g2d_format = G2D_COLOR_FMT_XRGB1555|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_XBGR1555,
		.g2d_format = G2D_COLOR_FMT_XRGB1555|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBX5551,
		.g2d_format = G2D_COLOR_FMT_XRGB1555|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRX5551,
		.g2d_format = G2D_COLOR_FMT_XRGB1555|G2D_ORDER_BGRAX,
	},
	{
		.drm_format = DRM_FORMAT_ARGB1555,
		.g2d_format = G2D_COLOR_FMT_ARGB1555|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_ABGR1555,
		.g2d_format = G2D_COLOR_FMT_ARGB1555|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBA5551,
		.g2d_format = G2D_COLOR_FMT_ARGB1555|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRA5551,
		.g2d_format = G2D_COLOR_FMT_ARGB1555|G2D_ORDER_BGRAX,
	},
	{
		.drm_format = DRM_FORMAT_RGB565,
		.g2d_format = G2D_COLOR_FMT_RGB565|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_BGR565,
		.g2d_format = G2D_COLOR_FMT_RGB565|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGB888,
		.g2d_format = G2D_COLOR_FMT_PRGB888|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_BGR888,
		.g2d_format = G2D_COLOR_FMT_PRGB888|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_XRGB8888,
		.g2d_format = G2D_COLOR_FMT_XRGB8888|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_XBGR8888,
		.g2d_format = G2D_COLOR_FMT_XRGB8888|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBX8888,
		.g2d_format = G2D_COLOR_FMT_XRGB8888|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRX8888,
		.g2d_format = G2D_COLOR_FMT_XRGB8888|G2D_ORDER_BGRAX,
	},
	{
		.drm_format = DRM_FORMAT_ARGB8888,
		.g2d_format = G2D_COLOR_FMT_ARGB8888|G2D_ORDER_AXRGB,
	},
	{
		.drm_format = DRM_FORMAT_ABGR8888,
		.g2d_format = G2D_COLOR_FMT_ARGB8888|G2D_ORDER_AXBGR,
	},
	{
		.drm_format = DRM_FORMAT_RGBA8888,
		.g2d_format = G2D_COLOR_FMT_ARGB8888|G2D_ORDER_RGBAX,
	},
	{
		.drm_format = DRM_FORMAT_BGRA8888,
		.g2d_format = G2D_COLOR_FMT_ARGB8888|G2D_ORDER_BGRAX,
	},
};

uint32_t get_g2d_format_from_drm(uint32_t fmt) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
		if (formats[i].drm_format == fmt) {
			return formats[i].g2d_format;
		}
	}

	wlr_log(WLR_ERROR, "DRM format 0x%"PRIX32" has no G2D equivalent", fmt);
	return 0;
}

uint32_t get_drm_format_from_g2d(uint32_t fmt) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
		if (formats[i].g2d_format == fmt) {
			return formats[i].drm_format;
		}
	}

	wlr_log(WLR_ERROR, "G2D format 0x%"PRIX32" has no drm equivalent", fmt);
	return DRM_FORMAT_INVALID;
}

const uint32_t *get_g2d_drm_formats(size_t *len) {
	static uint32_t drm_formats[sizeof(formats) / sizeof(formats[0])];
	*len = sizeof(formats) / sizeof(formats[0]);
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
		drm_formats[i] = formats[i].drm_format;
	}
	return drm_formats;
}
