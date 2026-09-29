// src/reporter-capture.c - Frame capture and processing for Kanami Reporter filter.

#include "reporter-capture.h"

#include <util/bmem.h>

#include <string.h>

void reporter_frame_free(struct frame_data *frame)
{
	if (!frame)
		return;

	bfree(frame->bgra);
	bfree(frame);
}

void reporter_capture_frame(struct kanami_reporter_data *data,
			    uint64_t now,
			    bool enabled,
			    double threshold)
{
	obs_source_t *target = obs_filter_get_target(data->source);
	obs_source_t *parent = obs_filter_get_parent(data->source);

	if (!target || !parent || !data->capture || !data->stage)
		return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	if (!width || !height)
		return;

	gs_texrender_reset(data->capture);

	if (!gs_texrender_begin(data->capture, FRAME_W, FRAME_H))
		return;

	struct vec4 clear = {0};
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height,
		 -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	uint32_t flags = obs_source_get_output_flags(target);

	/*
	 * 同步父来源直接渲染，避免再次进入其滤镜链。
	 */
	if (target == parent &&
	    !(flags & OBS_SOURCE_ASYNC) &&
	    !(flags & OBS_SOURCE_CUSTOM_DRAW)) {
		obs_source_default_render(target);
	} else {
		obs_source_video_render(target);
	}

	gs_blend_state_pop();
	gs_texrender_end(data->capture);

	gs_stage_texture(data->stage,
			 gs_texrender_get_texture(data->capture));

	uint8_t *pixels = NULL;
	uint32_t stride = 0;

	if (!gs_stagesurface_map(data->stage, &pixels, &stride))
		return;

	struct frame_data *frame = bzalloc(sizeof(*frame));
	frame->bgra = bmalloc(FRAME_BYTES);
	frame->width = FRAME_W;
	frame->height = FRAME_H;
	frame->timestamp = now;
	frame->recognize = enabled;
	frame->threshold = threshold;

	for (size_t y = 0; y < FRAME_H; y++) {
		memcpy(frame->bgra + y * FRAME_W * 4,
		       pixels + y * stride, FRAME_W * 4);
	}

	gs_stagesurface_unmap(data->stage);

	EnterCriticalSection(&data->lock);

	struct frame_data *old = data->pending;
	data->pending = frame;

	if (old)
		data->dropped++;

	LeaveCriticalSection(&data->lock);

	reporter_frame_free(old);
	SetEvent(data->wake);
}
