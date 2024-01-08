drm = dependency('libdrm', required: 'g2d' in renderers)
drm_exynos = dependency('libdrm_exynos',
 	version: '>=0.7',
	required: 'g2d' in renderers)

wlr_files += files(
	'pixel_format.c',
	'renderer.c',
)

wlr_deps += drm
wlr_deps += drm_exynos
features += { 'g2d-renderer': true }
