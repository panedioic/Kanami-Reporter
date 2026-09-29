// src/reporter-settings.c - Settings management for Kanami Reporter filter.

#include "reporter-settings.h"
#include "reporter-states.h"
#include "reporter-template.h"

#include <util/bmem.h>

#include <stdio.h>

int reporter_setting_int(obs_data_t *settings, const char *name,
			 int minimum, int maximum)
{
	long long value = obs_data_get_int(settings, name);

	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;

	return (int)value;
}

static void roi_setting_key(char *buffer, size_t size,
			    int state_id, const char *field)
{
	snprintf(buffer, size, "event_roi_%s_%s", state_names[state_id], field);
}

static void remember_event_roi(obs_data_t *settings, int state_id)
{
	if (state_id < 0 || state_id >= STATE_COUNT)
		return;

	static const char *fields[] = {
		"roi_x", "roi_y", "roi_width", "roi_height"
	};

	char key[128];

	for (size_t i = 0; i < 4; i++) {
		roi_setting_key(key, sizeof(key), state_id, fields[i]);
		obs_data_set_int(settings, key,
				 obs_data_get_int(settings, fields[i]));
	}
}

static void restore_event_roi(obs_data_t *settings, int state_id)
{
	if (state_id < 0 || state_id >= STATE_COUNT)
		return;

	static const char *fields[] = {
		"roi_x", "roi_y", "roi_width", "roi_height"
	};

	char key[128];
	bool has_saved_settings = true;

	for (size_t i = 0; i < 4; i++) {
		roi_setting_key(key, sizeof(key), state_id, fields[i]);

		if (!obs_data_has_user_value(settings, key)) {
			has_saved_settings = false;
			break;
		}
	}

	if (has_saved_settings) {
		for (size_t i = 0; i < 4; i++) {
			roi_setting_key(key, sizeof(key), state_id, fields[i]);
			obs_data_set_int(settings, fields[i],
					 obs_data_get_int(settings, key));
		}
		return;
	}

	/*
	 * 兼容已有模板：
	 * 该事件还没有界面设置时，读取模板中保存的ROI。
	 */
	struct reporter_roi restored = {840, 170, 240, 60};
	struct template_data model = {0};

	char *directory = obs_module_config_path("templates");

	if (directory) {
		char *path = reporter_template_path(directory, state_id);

		if (reporter_template_load(path, &model)) {
			restored = model.roi;
			reporter_template_free(&model);
		}

		bfree(path);
		bfree(directory);
	}

	obs_data_set_int(settings, "roi_x", restored.x);
	obs_data_set_int(settings, "roi_y", restored.y);
	obs_data_set_int(settings, "roi_width", restored.w);
	obs_data_set_int(settings, "roi_height", restored.h);

	remember_event_roi(settings, state_id);
}

void reporter_settings_sync_event_roi(obs_data_t *settings,
				      int *selected_cue,
				      struct reporter_roi *selected_roi)
{
	int selected = reporter_setting_int(
		settings, "template_event", 0, STATE_COUNT - 1);

	if (!obs_data_has_user_value(settings, "roi_display_event")) {
		restore_event_roi(settings, selected);
		obs_data_set_int(settings, "roi_display_event", selected);
	} else {
		int displayed = reporter_setting_int(
			settings, "roi_display_event", 0, STATE_COUNT - 1);

		if (displayed != selected) {
			remember_event_roi(settings, displayed);
			restore_event_roi(settings, selected);
			obs_data_set_int(settings, "roi_display_event", selected);
		}
	}

	struct reporter_roi roi = {
		reporter_setting_int(settings, "roi_x", 0, FRAME_W - 1),
		reporter_setting_int(settings, "roi_y", 0, FRAME_H - 1),
		reporter_setting_int(settings, "roi_width", 4, FRAME_W),
		reporter_setting_int(settings, "roi_height", 4, FRAME_H),
	};

	/* 保持界面显示与工作线程使用的坐标一致。 */
	obs_data_set_int(settings, "roi_x", roi.x);
	obs_data_set_int(settings, "roi_y", roi.y);
	obs_data_set_int(settings, "roi_width", roi.w);
	obs_data_set_int(settings, "roi_height", roi.h);
	remember_event_roi(settings, selected);

	if (selected_cue)
		*selected_cue = selected;

	if (selected_roi)
		*selected_roi = roi;
}

bool reporter_settings_event_changed(obs_properties_t *props,
				     obs_property_t *property,
				     obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	int next = reporter_setting_int(
		settings, "template_event", 0, STATE_COUNT - 1);

	/*
	 * OBS在打开对话框时也可能调用该回调。
	 * 选择没有变化时不覆盖当前编辑内容。
	 */
	if (!obs_data_has_user_value(settings, "roi_display_event")) {
		restore_event_roi(settings, next);
		obs_data_set_int(settings, "roi_display_event", next);
		return true;
	}

	int previous = reporter_setting_int(
		settings, "roi_display_event", 0, STATE_COUNT - 1);

	if (previous == next)
		return false;

	/* 界面上显示的坐标仍然属于上一个事件。 */
	remember_event_roi(settings, previous);
	restore_event_roi(settings, next);
	obs_data_set_int(settings, "roi_display_event", next);

	/* 让OBS刷新属性控件。 */
	return true;
}
