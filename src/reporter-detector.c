// src/reporter-detector.c - State detection for Kanami Reporter filter.

#include "reporter-detector.h"
#include "reporter-states.h"
#include "reporter-context.h"
#include "reporter-template.h"

#include <stddef.h>
#include <string.h>

const unsigned int reporter_all_state_count = (unsigned int)STATE_COUNT;

/*
 * 每个数组下标对应state_names中的同一下标。
 * 由模板加载/保存逻辑设置ROI。
 */
static struct reporter_roi state_rois[STATE_COUNT];
static bool state_roi_configured[STATE_COUNT];
static bool state_matches[STATE_COUNT];

static int find_state(const char *state_name)
{
	if (!state_name)
		return -1;

	for (unsigned int i = 0; i < reporter_all_state_count; i++) {
		if (strcmp(state_names[i], state_name) == 0)
			return (int)i;
	}

	return -1;
}

void reporter_detector_begin_frame(void)
{
	memset(state_matches, 0, sizeof(state_matches));
}

void reporter_detector_set_result(const char *state_name, bool matched)
{
	int index = find_state(state_name);

	if (index >= 0)
		state_matches[index] = matched;
}

bool reporter_get_state_roi(const char *state_name,
			    struct reporter_roi *roi)
{
	if (!roi)
		return false;

	memset(roi, 0, sizeof(*roi));

	int index = find_state(state_name);

	if (index < 0 || !state_roi_configured[index])
		return false;

	*roi = state_rois[index];
	return true;
}

void reporter_set_state_roi(const char *state_name,
			    struct reporter_roi roi)
{
	int index = find_state(state_name);

	if (index < 0)
		return;

	state_rois[index] = roi;
	state_roi_configured[index] =
		roi.x >= 0 && roi.y >= 0 && roi.w > 0 && roi.h > 0;
}

/*
 * 检查当前帧是否是某个特定状态。
 * param gray 灰度图像数据，大小为width*height
 * param width 图像宽度
 * param height 图像高度
 * param state_name 待判断的状态名称
 */
bool reporter_check_state(void *argument, enum state_id state_id)
{
	struct kanami_reporter_data *data = argument;

	if (!data || (unsigned int)state_id >= (unsigned int)STATE_COUNT)
		return false;

	const struct frame_data *frame = data->processing_frame;

	if (!frame || !frame->gray ||
	    frame->width != FRAME_W || frame->height != FRAME_H)
		return false;

	const struct template_data *model = &data->models[state_id];

	struct reporter_roi roi = {0};
	bool matched = false;

	if (model->pixels) {
		roi = model->roi;

		double score = reporter_template_score(model, frame->gray);
		matched = score >= frame->threshold;
	}

	reporter_set_state_roi(state_names[state_id], roi);
	reporter_detector_set_result(state_names[state_id], matched);

	return matched;
}

int reporter_get_player_kill(const uint8_t *gray,
			    uint32_t width,
			    uint32_t height)
{
	(void)gray;
	(void)width;
	(void)height;

	/* TODO: 识别玩家本回合击杀数。 */
	return 2;
}

void reporter_get_game_score(const uint8_t *gray,
			     uint32_t width,
			     uint32_t height,
			     int *us,
			     int *enemy)
{
	(void)gray;
	(void)width;
	(void)height;

	/* TODO: OCR读取双方比分。 */
	if (us)
		*us = 3;

	if (enemy)
		*enemy = 3;
}

void reporter_get_target_score(const uint8_t *gray,
			     uint32_t width,
				 uint32_t height,
			     int *target)
{
	(void)gray;
	(void)width;
	(void)height;

	if (target)
		*target = 10;
}

void reporter_get_alive_players(const uint8_t *gray,
				uint32_t width,
				uint32_t height,
				int *us,
				int *enemy)
{
	(void)gray;
	(void)width;
	(void)height;

	/* TODO: 读取双方存活人数。 */
	if (us)
		*us = 2;

	if (enemy)
		*enemy = 2;
}

bool reporter_get_bomber_alive(const uint8_t *gray,
			      uint32_t width,
			      uint32_t height)
{
	(void)gray;
	(void)width;
	(void)height;

	/*
	 * TODO: 判断炸弹携带者是否存活。
	 * 未实现时返回true，避免占位实现启动即播报携带者倒地。
	 */
	return true;
}
