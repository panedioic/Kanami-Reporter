// src/reporter-properties.c - Property definitions for Kanami Reporter filter.

#include "reporter-properties.h"
#include "reporter-context.h"
#include "reporter-states.h"
#include "reporter-settings.h"
#include "reporter-import.h"

#include <util/platform.h>

static bool request_template(obs_properties_t *props,
			     obs_property_t *property, void *private_data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = private_data;
	if (!data)
		return false;

	EnterCriticalSection(&data->lock);
	data->train_request = data->selected_cue;
	data->train_roi = data->selected_roi;
	data->train_after = os_gettime_ns();
	LeaveCriticalSection(&data->lock);

	blog(LOG_INFO,
	     "[Kanami-Reporter] Template capture requested. "
	     "Keep the source visible.");

	return false;
}

static bool request_preview(obs_properties_t *props,
			    obs_property_t *property, void *private_data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = private_data;
	if (!data)
		return false;

	EnterCriticalSection(&data->lock);
	data->preview_requested = true;
	data->preview_roi = data->selected_roi;
	data->preview_after = os_gettime_ns();
	LeaveCriticalSection(&data->lock);

	blog(LOG_INFO,
	     "[Kanami-Reporter] ROI preview requested. "
	     "Keep the source visible.");

	return false;
}

static bool request_import_resources(obs_properties_t *props,
				     obs_property_t *property,
				     void *private_data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = private_data;
	if (!data)
		return false;

	if (!reporter_import_resources_from_dialog())
		return false;

	EnterCriticalSection(&data->lock);
	data->reload_requested = true;
	LeaveCriticalSection(&data->lock);

	SetEvent(data->wake);
	return false;
}

void reporter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "recognition_enabled", true);
	obs_data_set_default_double(settings, "match_threshold", 0.90);
	obs_data_set_default_int(settings, "template_event", STATE_ROUND_END_WIN);
	obs_data_set_default_int(settings, "roi_x", 840);
	obs_data_set_default_int(settings, "roi_y", 170);
	obs_data_set_default_int(settings, "roi_width", 240);
	obs_data_set_default_int(settings, "roi_height", 60);
}

obs_properties_t *reporter_properties(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(
		props, "recognition_enabled", "Enable recognition");

	obs_properties_add_float_slider(
		props, "match_threshold", "Template match threshold",
		0.50, 0.999, 0.001);

	obs_property_t *events = obs_properties_add_list(
		props, "template_event", "Template event",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	for (int i = 0; i < STATE_COUNT; i++)
		obs_property_list_add_int(events, state_names[i], i);

	obs_property_set_modified_callback(
		events, reporter_settings_event_changed);

	obs_properties_add_int(
		props, "roi_x", "Template X (1920x1080)", 0, FRAME_W - 1, 1);
	obs_properties_add_int(
		props, "roi_y", "Template Y (1920x1080)", 0, FRAME_H - 1, 1);
	obs_properties_add_int(
		props, "roi_width", "Template width", 4, FRAME_W, 1);
	obs_properties_add_int(
		props, "roi_height", "Template height", 4, FRAME_H, 1);

	obs_properties_add_button(
		props, "preview_roi", "Save ROI preview (red rectangle)",
		request_preview);

	obs_properties_add_button(
		props, "save_template", "Save / replace event template",
		request_template);

	obs_properties_add_button(
		props, "import_resources",
		"Import resource package (zip)",
		request_import_resources);

	obs_properties_add_text(
		props, "help",
		"Coordinates use a 1920x1080 image. "
		"Save an ROI preview to check the position before saving a "
		"template. Each event keeps its own ROI. "
		"Saving replaces that event's template and resets "
		"recognition state.",
		OBS_TEXT_INFO);

	return props;
}