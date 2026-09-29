// src/reporter-worker.c - Worker thread for Kanami Reporter filter.

#include "reporter-worker.h"
#include "reporter-states.h"
#include "reporter-detector.h"
#include "reporter-statemachine.h"
#include "reporter-template.h"

#include <util/bmem.h>
#include <util/platform.h>

#include <process.h>
#include <string.h>

/* ---------- 模板训练：仅在工作线程执行 ---------- */

static void train_template(struct kanami_reporter_data *data,
			   struct template_data models[STATE_COUNT],
			   int state_id, struct reporter_roi roi,
			   const uint8_t *gray)
{
	if ((unsigned int)state_id >= (unsigned int)STATE_COUNT)
		return;

	if (!reporter_valid_roi(roi)) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] Invalid training ROI: "
		     "x=%d y=%d w=%d h=%d; frame=%dx%d; max_area=%d",
		     roi.x, roi.y, roi.w, roi.h,
		     FRAME_W, FRAME_H, MAX_TEMPLATE_PIXELS);
		return;
	}

	struct template_data model = {0};
	model.roi = roi;
	model.pixels = bmalloc((size_t)roi.w * (size_t)roi.h);

	for (int y = 0; y < roi.h; y++) {
		memcpy(model.pixels + (size_t)y * (size_t)roi.w,
		       gray + (size_t)(roi.y + y) * FRAME_W + roi.x,
		       (size_t)roi.w);
	}

	char *path = reporter_template_path(data->template_dir, state_id);

	/* 通过保存再加载的方式复用统一校验。 */
	bool saved = reporter_template_save(path, &model);

	if (saved) {
		struct template_data verified = {0};

		if (reporter_template_load(path, &verified)) {
			reporter_template_free(&models[state_id]);
			models[state_id] = verified;
		} else {
			saved = false;
		}
	}

	if (saved) {
		blog(LOG_INFO,
		     "[Kanami-Reporter] [TEMPLATE] saved=%s "
		     "roi=%d,%d,%d,%d path=%s",
		     state_names[state_id], roi.x, roi.y, roi.w, roi.h, path);
	} else {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] Template rejected or not saved "
		     "(almost uniform ROI?): %s", path);
	}

	reporter_template_free(&model);
	bfree(path);
}

/* ---------- 工作线程 ---------- */

unsigned int __stdcall reporter_worker(void *argument)
{
	struct kanami_reporter_data *data = argument;
	// struct template_data models[STATE_COUNT] = {0};

	uint8_t *gray = bmalloc((size_t)FRAME_W * FRAME_H);
	int loaded = 0;

	reporter_state_reset();

	for (int i = 0; i < STATE_COUNT; i++) {
		char *path = reporter_template_path(data->template_dir, i);

		if (reporter_template_load(path, &data->models[i]))
			loaded++;

		bfree(path);
	}

	blog(LOG_INFO,
	     "[Kanami-Reporter] Loaded %d templates from %s",
	     loaded, data->template_dir);

	bool was_enabled = false;
	uint64_t processed = 0;
	uint64_t report_since = os_gettime_ns();
	uint64_t work_ns = 0;
	uint64_t previous_dropped = 0;

	for (;;) {
		WaitForSingleObject(data->wake, INFINITE);

		EnterCriticalSection(&data->lock);

		bool stop = data->stop;
		struct frame_data *frame = data->pending;
		data->pending = NULL;

		int train = -1;
		struct reporter_roi train_roi = {0};

		bool preview = false;
		struct reporter_roi preview_roi = {0};

		/*
		 * 只在帧足够新时消费按钮请求，
		 * 避免请求被过期帧吞掉。
		 */
		bool fresh = frame &&
			os_gettime_ns() - frame->timestamp <= 500000000ULL;

		if (fresh && data->train_request >= 0 &&
		    frame->timestamp >= data->train_after) {
			train = data->train_request;
			train_roi = data->train_roi;
			data->train_request = -1;
		}

		if (fresh && data->preview_requested &&
		    frame->timestamp >= data->preview_after) {
			preview = true;
			preview_roi = data->preview_roi;
			data->preview_requested = false;
		}

		uint64_t dropped = data->dropped;

		LeaveCriticalSection(&data->lock);

		if (stop) {
			reporter_frame_free(frame);
			break;
		}

		if (!frame)
			continue;

		if (!fresh) {
			reporter_frame_free(frame);
			continue;
		}

		uint64_t started = os_gettime_ns();

		if (preview)
			reporter_save_roi_preview(frame, preview_roi);

		reporter_to_grayscale(frame->bgra, gray);

		frame->gray = gray;
		data->processing_frame = frame;

		if (train >= 0) {
			train_template(data, data->models, train, train_roi, gray);
			reporter_state_reset();
			was_enabled = false;

		} else if (frame->recognize) {
			if (!was_enabled)
				reporter_state_reset();

			reporter_detector_begin_frame();
            
			reporter_state_process_frame(data);
			was_enabled = true;

		} else {
			was_enabled = false;
		}

		work_ns += os_gettime_ns() - started;
		processed++;

		uint64_t now = os_gettime_ns();

		if (now - report_since >= 10000000000ULL) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [PERF] processed=%llu "
			     "dropped=%llu avg_worker_ms=%.2f state=%s",
			     (unsigned long long)processed,
			     (unsigned long long)(dropped - previous_dropped),
			     processed ? (double)work_ns /
					 (double)processed / 1000000.0 : 0.0,
			     reporter_state_get_current());

			processed = 0;
			work_ns = 0;
			report_since = now;
			previous_dropped = dropped;
		}

		data->processing_frame = NULL;
		reporter_frame_free(frame);
	}

	data->processing_frame = NULL;

	for (int i = 0; i < STATE_COUNT; i++)
		reporter_template_free(&data->models[i]);

	bfree(gray);
	return 0;
}
