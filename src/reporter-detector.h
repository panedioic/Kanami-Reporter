// src/reporter-detector.h - State detection for Kanami Reporter filter.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "reporter-types.h"
#include "reporter-states.h"

extern const unsigned int reporter_all_state_count;

/*
 * 每张截图开始处理时调用，清空上一帧的状态判断结果。
 * 之后由已有模板识别器填入结果。
 */
void reporter_detector_begin_frame(void);

void reporter_detector_set_result(const char *state_name, bool matched);

bool reporter_check_state(void *argument, enum state_id state_id);

/*
 * 返回该状态判断器对应的ROI。
 * 没有配置时返回false，并将roi清零。
 */
bool reporter_get_state_roi(const char *state_name,
			    struct reporter_roi *roi);

void reporter_set_state_roi(const char *state_name,
			    struct reporter_roi roi);

/* 状态识别函数（击败/得分/目标分数/炸弹携带者状态）。 */
int reporter_get_player_kill(const uint8_t *gray,
			    uint32_t width,
			    uint32_t height);

void reporter_get_game_score(const uint8_t *gray,
			     uint32_t width,
			     uint32_t height,
			     int *us,
			     int *enemy);

void reporter_get_target_score(const uint8_t *gray,
			       uint32_t width,
			       uint32_t height,
			       int *target);

void reporter_get_alive_players(const uint8_t *gray,
				uint32_t width,
				uint32_t height,
				int *us,
				int *enemy);

bool reporter_get_bomber_alive(const uint8_t *gray,
			      uint32_t width,
			      uint32_t height);
