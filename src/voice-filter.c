#include "voice-filter.h"

#include <graphics/graphics.h>
#include <util/platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kanami_reporter_data {
	obs_source_t *source;

	int x;
	int y;
	int width;
	int height;

	gs_texrender_t *capture;
	gs_stagesurf_t *stage;
	uint32_t stage_width;
	uint32_t stage_height;

	bool save_requested;
	char *save_path;
	char *status;
};

static const char *filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Kanami Reporter";
}

static void set_status(struct kanami_reporter_data *data,
		       const char *message)
{
	bfree(data->status);
	data->status = bstrdup(message);
}

static void filter_update(void *priv_data, obs_data_t *settings)
{
	struct kanami_reporter_data *data = priv_data;

	data->x = (int)obs_data_get_int(settings, "roi_x");
	data->y = (int)obs_data_get_int(settings, "roi_y");
	data->width = (int)obs_data_get_int(settings, "roi_width");
	data->height = (int)obs_data_get_int(settings, "roi_height");

	if (data->x < 0)
		data->x = 0;
	if (data->y < 0)
		data->y = 0;
	if (data->width < 1)
		data->width = 1;
	if (data->height < 1)
		data->height = 1;
}

/* 显式写入 little-endian BMP 头，避免结构体对齐问题。 */
static void write_u16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
}

static bool save_bmp(const char *path, const uint8_t *pixels,
		     uint32_t stride, uint32_t width, uint32_t height)
{
	const uint32_t row_size = width * 4;
	const uint32_t pixel_size = row_size * height;
	uint8_t header[54] = {0};

	header[0] = 'B';
	header[1] = 'M';
	write_u32(header + 2, 54 + pixel_size);
	write_u32(header + 10, 54);
	write_u32(header + 14, 40);
	write_u32(header + 18, width);

	/* 负高度表示 top-down，保持截图的上下方向。 */
	write_u32(header + 22, (uint32_t)(-(int32_t)height));
	write_u16(header + 26, 1);
	write_u16(header + 28, 32);
	write_u32(header + 34, pixel_size);

	FILE *file = os_fopen(path, "wb");
	if (!file)
		return false;

	bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);

	for (uint32_t y = 0; ok && y < height; y++) {
		ok = fwrite(pixels + (size_t)y * stride, 1, row_size, file)
		     == row_size;
	}

	if (fclose(file) != 0)
		ok = false;

	return ok;
}

static bool request_save(obs_properties_t *props, obs_property_t *property,
			 void *priv_data)
{
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = priv_data;
	if (!data)
		return false;

	data->save_requested = true;

	obs_property_t *status = obs_properties_get(props, "status");
	obs_property_set_description(
		status,
		"Capture requested. Keep the source visible, then reopen "
		"this dialog to see the result.");

	return true;
}

static obs_properties_t *filter_properties(void *priv_data)
{
	struct kanami_reporter_data *data = priv_data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "roi_x", "ROI X", 0, 16384, 1);
	obs_properties_add_int(props, "roi_y", "ROI Y", 0, 16384, 1);
	obs_properties_add_int(props, "roi_width", "ROI width", 1, 4096, 1);
	obs_properties_add_int(props, "roi_height", "ROI height", 1, 4096, 1);

	obs_properties_add_button(props, "save_roi", "Save ROI to BMP",
				  request_save);

	obs_properties_add_text(
		props, "status",
		data && data->status ? data->status : "Ready.",
		OBS_TEXT_INFO);

	return props;
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "roi_x", 0);
	obs_data_set_default_int(settings, "roi_y", 0);
	obs_data_set_default_int(settings, "roi_width", 640);
	obs_data_set_default_int(settings, "roi_height", 120);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct kanami_reporter_data *data = bzalloc(sizeof(*data));
	data->source = source;

	/*
	 * 存放到 OBS 插件配置目录。
	 * 不向 Program Files 写入运行时截图。
	 */
	char *config_dir = obs_module_config_path("");
	if (config_dir) {
		os_mkdirs(config_dir);
		bfree(config_dir);
	}

	data->save_path = obs_module_config_path("roi-debug.bmp");
	set_status(data, "Ready. Click Save ROI to capture the selected region.");

	obs_enter_graphics();
	data->capture = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	obs_leave_graphics();

	if (!data->capture || !data->save_path) {
		obs_enter_graphics();
		if (data->capture)
			gs_texrender_destroy(data->capture);
		obs_leave_graphics();

		bfree(data->save_path);
		bfree(data->status);
		bfree(data);
		return NULL;
	}

	filter_update(data, settings);
	return data;
}

static void filter_destroy(void *priv_data)
{
	struct kanami_reporter_data *data = priv_data;
	if (!data)
		return;

	obs_enter_graphics();
	if (data->stage)
		gs_stagesurface_destroy(data->stage);
	if (data->capture)
		gs_texrender_destroy(data->capture);
	obs_leave_graphics();

	bfree(data->save_path);
	bfree(data->status);
	bfree(data);
}

static void capture_roi(struct kanami_reporter_data *data)
{
	obs_source_t *target = obs_filter_get_target(data->source);
	obs_source_t *parent = obs_filter_get_parent(data->source);

	if (!target || !parent) {
		set_status(data, "Capture failed: no source.");
		return;
	}

	const uint32_t source_width = obs_source_get_base_width(target);
	const uint32_t source_height = obs_source_get_base_height(target);

	if (!source_width || !source_height) {
		set_status(data, "Capture failed: source has no video dimensions.");
		return;
	}

	const uint32_t x = (uint32_t)data->x;
	const uint32_t y = (uint32_t)data->y;

	if (x >= source_width || y >= source_height) {
		set_status(data, "Capture failed: ROI origin is outside the source.");
		return;
	}

	uint32_t width = (uint32_t)data->width;
	uint32_t height = (uint32_t)data->height;

	/* 防止越界；截图尺寸上限也限制了 BMP 大小。 */
	if (width > 4096)
		width = 4096;
	if (height > 4096)
		height = 4096;
	if (width > source_width - x)
		width = source_width - x;
	if (height > source_height - y)
		height = source_height - y;

	gs_texrender_reset(data->capture);

	if (!gs_texrender_begin(data->capture, width, height)) {
		set_status(data, "Capture failed: could not create render target.");
		return;
	}

	struct vec4 clear_color = {0};
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	/*
	 * 只把 ROI 范围投影到小纹理。
	 * 坐标相对于来源，不是 OBS 输出画布。
	 */
	gs_ortho((float)x, (float)(x + width),
		 (float)y, (float)(y + height), -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	/*
	 * 直接渲染同步父来源时使用 default_render，
	 * 避免再次进入其滤镜链。
	 */
	const uint32_t flags = obs_source_get_output_flags(target);
	if (target == parent &&
	    !(flags & OBS_SOURCE_ASYNC) &&
	    !(flags & OBS_SOURCE_CUSTOM_DRAW)) {
		obs_source_default_render(target);
	} else {
		obs_source_video_render(target);
	}

	gs_blend_state_pop();
	gs_texrender_end(data->capture);

	if (!data->stage ||
	    data->stage_width != width ||
	    data->stage_height != height) {
		if (data->stage)
			gs_stagesurface_destroy(data->stage);

		data->stage = gs_stagesurface_create(width, height, GS_BGRA);
		data->stage_width = width;
		data->stage_height = height;
	}

	if (!data->stage) {
		set_status(data, "Capture failed: could not create staging surface.");
		return;
	}

	gs_stage_texture(data->stage, gs_texrender_get_texture(data->capture));

	uint8_t *pixels = NULL;
	uint32_t stride = 0;

	if (!gs_stagesurface_map(data->stage, &pixels, &stride)) {
		set_status(data, "Capture failed: could not read pixels.");
		return;
	}

	bool saved = save_bmp(data->save_path, pixels, stride, width, height);
	gs_stagesurface_unmap(data->stage);

	if (saved) {
		set_status(data, data->save_path);
		blog(LOG_INFO, "[Kanami-Reporter] ROI saved: %s (%ux%u)",
		     data->save_path, width, height);
	} else {
		set_status(data, "Capture failed: could not write BMP.");
		blog(LOG_WARNING, "[Kanami-Reporter] Cannot write: %s",
		     data->save_path);
	}
}

static void filter_render(void *priv_data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct kanami_reporter_data *data = priv_data;

	if (data->save_requested) {
		data->save_requested = false;
		capture_roi(data);
	}

	/* 正常画面保持原样。 */
	obs_source_skip_video_filter(data->source);
}

struct obs_source_info kanami_reporter_filter = {
	.id = "kanami_reporter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = filter_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.update = filter_update,
	.video_render = filter_render,
};