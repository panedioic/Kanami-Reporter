/*
Kanami-Reporter
Copyright (C) 2026 Yiyan me@y1yan.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "voice-filter.h"
#include "reporter-capture.h"
#include "reporter-context.h"
#include "reporter-properties.h"
#include "reporter-settings.h"
#include "reporter-worker.h"

#include <graphics/graphics.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <process.h>
#include <string.h>

static const char *reporter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Kanami Reporter";
}

static void reporter_update(void *private_data, obs_data_t *settings)
{
	struct kanami_reporter_data *data = private_data;

	int selected = 0;
	struct reporter_roi selected_roi = {0};

	reporter_settings_sync_event_roi(
		settings, &selected, &selected_roi);

	bool enabled = obs_data_get_bool(settings, "recognition_enabled");

	double threshold = obs_data_get_double(settings, "match_threshold");
	if (!(threshold >= 0.50 && threshold <= 0.999))
		threshold = 0.90;

	EnterCriticalSection(&data->lock);

	data->enabled = enabled;
	data->threshold = threshold;
	data->selected_cue = selected;
	data->selected_roi = selected_roi;

	LeaveCriticalSection(&data->lock);
}

static void *reporter_create(obs_data_t *settings, obs_source_t *source)
{
	struct kanami_reporter_data *data = bzalloc(sizeof(*data));

	data->source = source;
	data->train_request = -1;

	InitializeCriticalSection(&data->lock);
	data->wake = CreateEventW(NULL, FALSE, FALSE, NULL);

	char *directory = obs_module_config_path("templates");
	data->template_dir = directory;

	if (directory)
		os_mkdirs(directory);

	obs_enter_graphics();
	data->capture = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	data->stage = gs_stagesurface_create(FRAME_W, FRAME_H, GS_BGRA);
	obs_leave_graphics();

	reporter_update(data, settings);

	if (data->wake && data->template_dir &&
	    data->capture && data->stage) {
		uintptr_t handle = _beginthreadex(
			NULL, 0, reporter_worker, data, 0, NULL);

		data->worker = (HANDLE)handle;
	}

	if (!data->worker) {
		obs_enter_graphics();
		if (data->stage)
			gs_stagesurface_destroy(data->stage);
		if (data->capture)
			gs_texrender_destroy(data->capture);
		obs_leave_graphics();

		if (data->wake)
			CloseHandle(data->wake);

		DeleteCriticalSection(&data->lock);
		bfree(data->template_dir);
		bfree(data);
		return NULL;
	}

	return data;
}

static void reporter_destroy(void *private_data)
{
	struct kanami_reporter_data *data = private_data;

	if (!data)
		return;

	EnterCriticalSection(&data->lock);
	data->stop = true;
	LeaveCriticalSection(&data->lock);

	SetEvent(data->wake);
	WaitForSingleObject(data->worker, INFINITE);

	CloseHandle(data->worker);
	CloseHandle(data->wake);
	reporter_frame_free(data->pending);

	obs_enter_graphics();
	gs_stagesurface_destroy(data->stage);
	gs_texrender_destroy(data->capture);
	obs_leave_graphics();

	DeleteCriticalSection(&data->lock);
	bfree(data->template_dir);
	bfree(data);
}

static void reporter_render(void *private_data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct kanami_reporter_data *data = private_data;
	uint64_t now = os_gettime_ns();

	EnterCriticalSection(&data->lock);
	bool enabled = data->enabled;
	double threshold = data->threshold;
	LeaveCriticalSection(&data->lock);

	/* 校准模板时也保持10Hz截图。 */
	if (!data->last_capture || now - data->last_capture >= CAPTURE_NS) {
		data->last_capture = now;
		reporter_capture_frame(data, now, enabled, threshold);
	}

	obs_source_skip_video_filter(data->source);
}

struct obs_source_info kanami_reporter_filter = {
	.id = "kanami_reporter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = reporter_name,
	.create = reporter_create,
	.destroy = reporter_destroy,
	.get_defaults = reporter_defaults,
	.get_properties = reporter_properties,
	.update = reporter_update,
	.video_render = reporter_render,
};
