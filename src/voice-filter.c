#include "voice-filter.h"
#include "plugin-support.h"

#include <util/platform.h>

#include <stdbool.h>
#include <stdint.h>

struct kanami_reporter_data {
	bool enabled;
	int interval_ms;
	int roi_x;
	int roi_y;
	int roi_width;
	int roi_height;

	uint64_t last_log_time_ns;
};

static void kanami_reporter_update(void *priv_data, obs_data_t *settings);

static const char *kanami_reporter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Kanami Reporter";
}

static void *kanami_reporter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);

	struct kanami_reporter_data *data =
		bzalloc(sizeof(struct kanami_reporter_data));

	data->enabled = true;
	data->interval_ms = 200;
	data->roi_x = 0;
	data->roi_y = 0;
	data->roi_width = 640;
	data->roi_height = 120;
	data->last_log_time_ns = 0;

	kanami_reporter_update(data, settings);

	return data;
}

static void kanami_reporter_destroy(void *priv_data)
{
	struct kanami_reporter_data *data = priv_data;
	bfree(data);
}

static void kanami_reporter_update(void *priv_data, obs_data_t *settings)
{
	struct kanami_reporter_data *data = priv_data;

	data->enabled = obs_data_get_bool(settings, "enabled");
	data->interval_ms = (int)obs_data_get_int(settings, "interval_ms");

	data->roi_x = (int)obs_data_get_int(settings, "roi_x");
	data->roi_y = (int)obs_data_get_int(settings, "roi_y");
	data->roi_width = (int)obs_data_get_int(settings, "roi_width");
	data->roi_height = (int)obs_data_get_int(settings, "roi_height");

	if (data->interval_ms < 50)
		data->interval_ms = 50;

	if (data->roi_width < 1)
		data->roi_width = 1;

	if (data->roi_height < 1)
		data->roi_height = 1;

	obs_log(
		LOG_INFO,
		"settings updated: enabled=%s interval=%dms roi=(%d,%d,%d,%d)",
		data->enabled ? "true" : "false",
		data->interval_ms,
		data->roi_x,
		data->roi_y,
		data->roi_width,
		data->roi_height
	);
}

static obs_properties_t *kanami_reporter_properties(void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(
		props,
		"enabled",
		"Enable recognition"
	);

	obs_properties_add_int(
		props,
		"interval_ms",
		"Recognition interval (ms)",
		50,
		5000,
		50
	);

	obs_properties_add_int(
		props,
		"roi_x",
		"ROI X",
		0,
		10000,
		1
	);

	obs_properties_add_int(
		props,
		"roi_y",
		"ROI Y",
		0,
		10000,
		1
	);

	obs_properties_add_int(
		props,
		"roi_width",
		"ROI width",
		1,
		10000,
		1
	);

	obs_properties_add_int(
		props,
		"roi_height",
		"ROI height",
		1,
		10000,
		1
	);

	return props;
}

static void kanami_reporter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "enabled", true);
	obs_data_set_default_int(settings, "interval_ms", 200);

	obs_data_set_default_int(settings, "roi_x", 0);
	obs_data_set_default_int(settings, "roi_y", 0);
	obs_data_set_default_int(settings, "roi_width", 640);
	obs_data_set_default_int(settings, "roi_height", 120);
}

static struct obs_source_frame *kanami_reporter_filter_video(
	void *priv_data,
	struct obs_source_frame *frame
)
{
	struct kanami_reporter_data *data = priv_data;

	if (!data || !frame || !data->enabled)
		return frame;

	uint64_t now = os_gettime_ns();
	uint64_t interval_ns = (uint64_t)data->interval_ms * 1000000ULL;

	if (data->last_log_time_ns == 0 ||
	    now - data->last_log_time_ns >= interval_ns) {
		data->last_log_time_ns = now;

		obs_log(
			LOG_INFO,
			"frame received: %ux%u format=%d roi=(%d,%d,%d,%d)",
			frame->width,
			frame->height,
			(int)frame->format,
			data->roi_x,
			data->roi_y,
			data->roi_width,
			data->roi_height
		);
	}

	return frame;
}

struct obs_source_info kanami_reporter_filter = {
	.id = "kanami_reporter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = kanami_reporter_get_name,
	.create = kanami_reporter_create,
	.destroy = kanami_reporter_destroy,
	.get_defaults = kanami_reporter_defaults,
	.get_properties = kanami_reporter_properties,
	.update = kanami_reporter_update,
	.filter_video = kanami_reporter_filter_video,
};