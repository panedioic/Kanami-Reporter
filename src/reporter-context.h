// src/reporter-context.h - Context definitions for Kanami Reporter filter.

#pragma once

#include <obs-module.h>

#include <windows.h>

#include <graphics/graphics.h>

#include "reporter-types.h"
#include "reporter-states.h"

/*
 * 一个识别滤镜实例的运行时数据。
 * 字段由属性线程和工作线程共享，访问时持有lock。
 */
struct kanami_reporter_data {
	obs_source_t *source;

	CRITICAL_SECTION lock;
	HANDLE wake;
	HANDLE worker;
	bool stop;

	bool enabled;
	double threshold;
	int selected_cue;
	struct reporter_roi selected_roi;

	int train_request;
	struct reporter_roi train_roi;
	uint64_t train_after;

	bool preview_requested;
	struct reporter_roi preview_roi;
	uint64_t preview_after;

	struct frame_data *pending;
	uint64_t dropped;

	uint64_t last_capture;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;

	/*
	 * 仅工作线程使用。
	 * 指向已从pending取走的帧，不能由截图线程修改或释放。
	 */
	struct frame_data *processing_frame;

    /* 模板文件存放目录 */
	char *template_dir;

    /* 模板识别器数据，按 state_id 索引。 */
    struct template_data models[STATE_COUNT];

	/* 资源导入完成后，通知工作线程重新加载模板。 */
	bool reload_requested;
};
