// src/reporter-statemachine.c - State machine for Kanami Reporter filter.

#include "reporter-statemachine.h"
#include "reporter-detector.h"
#include "reporter-trigger.h"
#include "reporter-states.h"
#include "reporter-context.h"

#include <obs-module.h>

#include <stdbool.h>
#include <string.h>

/* 当前实现对应一个识别滤镜实例。 */
static const char *current_state = "unknown";
static enum state_id current_state_id = STATE_COUNT;

/* 纳秒，使用截图时的单调时钟时间戳。 */
static uint64_t state_entered_at_ns = 0;
static double current_state_time = 0.0;

static int player_kill = 0;
static int game_score_us = 0;
static int game_score_enemy = 0;
static int game_score_target = 10;

static int alive_players_us = 0;
static int alive_players_enemy = 0;

static bool is_bomber_alive = false;

/* 0=未知，1=进攻方，2=防守方 */
static int current_side = 0;

static bool boomer_down_triggered = false;
static bool alive_players_us_1_triggered = false;
static bool alive_players_enemy_1_triggered = false;
static bool round_last_40s_triggered = false;
static bool round_last_20s_triggered = false;
static bool start_last_5s_triggered = false;

static bool state_is(const char *value, const char *expected)
{
	return strcmp(value, expected) == 0;
}

static void reset_round_triggers(void)
{
	boomer_down_triggered = false;
	alive_players_us_1_triggered = false;
	alive_players_enemy_1_triggered = false;
	round_last_40s_triggered = false;
	round_last_20s_triggered = false;
	start_last_5s_triggered = false;
}

void reporter_state_reset(void)
{
	current_state = "unknown";
	current_state_id = STATE_COUNT;

	player_kill = 0;
	game_score_us = 0;
	game_score_enemy = 0;
	current_state_time = 0.0;
	state_entered_at_ns = 0;

	alive_players_us = 0;
	alive_players_enemy = 0;

	is_bomber_alive = false;
	current_side = 0;

	reset_round_triggers();

	blog(LOG_INFO, "[Kanami-Reporter] [STATE] reset -> unknown");
}

void reporter_state_set_target_score(int target)
{
	if (target > 0)
		game_score_target = target;
}

const char *reporter_state_get_current(void)
{
	return current_state;
}

void reporter_state_process_frame(void *argument)
{
	struct kanami_reporter_data *data = argument;

	if (!data || !data->processing_frame)
		return;

	const struct frame_data *frame = data->processing_frame;
	const uint8_t *gray = frame->gray;
	uint32_t width = frame->width;
	uint32_t height = frame->height;

	if (!gray || !width || !height)
		return;

	/* 更新当前游戏状态信息 */
	const uint64_t now_ns = frame->timestamp;

	if (state_entered_at_ns != 0 && now_ns >= state_entered_at_ns) {
		current_state_time = (double)(now_ns - state_entered_at_ns) / 1000000000.0;
	} else {
		current_state_time = 0.0;
	}

	player_kill = reporter_get_player_kill(gray, width, height);

	reporter_get_game_score(
		gray, width, height,
		&game_score_us, &game_score_enemy);

	reporter_get_target_score(
		gray, width, height,
		&game_score_target);

	reporter_get_alive_players(
		gray, width, height,
		&alive_players_us, &alive_players_enemy);

	is_bomber_alive = reporter_get_bomber_alive(gray, width, height);

	/* 根据状态信息触发语音播报 */

	if (!is_bomber_alive && !boomer_down_triggered) {
		reporter_trigger("event_bomber_down");
		boomer_down_triggered = true;
	}

	if (alive_players_us == 1 && !alive_players_us_1_triggered) {
		reporter_trigger("event_alive_players_us_1");
		alive_players_us_1_triggered = true;
	}

	if (alive_players_enemy == 1 && !alive_players_enemy_1_triggered) {
		reporter_trigger("event_alive_players_enemy_1");
		alive_players_enemy_1_triggered = true;
	}

	if (current_state_id == STATE_ROUND_START && current_state_time >= 24.0 && !start_last_5s_triggered) {
		reporter_trigger("event_round_start_last_5s");
		start_last_5s_triggered = true;
	}

	if (current_state_id == STATE_ROUND_INGAME && current_state_time >= 75.0 && !round_last_40s_triggered) {
		reporter_trigger("event_round_ingame_last_40s");
		round_last_40s_triggered = true;
	}

	if (current_state_id == STATE_ROUND_INGAME && current_state_time >= 95.0 && !round_last_20s_triggered) {
		reporter_trigger("event_round_ingame_last_20s");
		round_last_20s_triggered = true;
	}

	/* 更新当前状态值 */

	const char *last_state = current_state;
	enum state_id last_state_id = current_state_id;

	for (unsigned int i = 0; i < reporter_all_state_count; i++) {
		const char *state_name = state_names[i];
		enum state_id detecting_state_id = (enum state_id)i;

		bool matched = reporter_check_state(data, detecting_state_id);

		if (!matched || detecting_state_id == last_state_id)
			continue;

		current_state = state_name;
		current_state_id = detecting_state_id;

		blog(LOG_INFO,
		     "[Kanami-Reporter] [STATE] %s -> %s "
		     "previous_duration=%.1fs kills=%d score=%d:%d "
		     "alive=%d:%d carrier=%d->%d",
		     last_state,
		     current_state,
		     current_state_time,
		     player_kill,
		     game_score_us,
		     game_score_enemy,
		     alive_players_us,
		     alive_players_enemy,
		     is_bomber_alive ? 1 : 0);

		if (current_state_id == STATE_GAME_START_ATTACKER) {
			reporter_trigger("event_game_start_attacker");
			current_side = 1;

		} else if (current_state_id == STATE_GAME_START_DEFENDER) {
			reporter_trigger("event_game_start_defender");
			current_side = 2;

		} else if (current_state_id == STATE_ROUND_START) {
			if (game_score_us + game_score_enemy ==
				    game_score_target - 2 &&
			    game_score_target == 10) {
				reporter_trigger("event_last_round_of_first_half");

			} else if (game_score_us + 1 == game_score_target ||
				   game_score_enemy + 1 == game_score_target) {
				reporter_trigger("event_decide_round");

			} else if ((game_score_us + 2 == game_score_target ||
				    game_score_enemy + 2 == game_score_target) &&
				   game_score_target > 10) {
				reporter_trigger("event_overtime_round");

			} else if (game_score_us + game_score_enemy ==
					   game_score_target - 1 &&
				   game_score_target == 10) {
				reporter_trigger("event_game_start_second_half");
			}

		} else if (current_state_id == STATE_ROUND_END_WIN ||
			   current_state_id == STATE_ROUND_END_LOSE) {
			if (last_state_id == STATE_ROUND_INGAME && current_state_time > 112.0) {
				reporter_trigger("event_victory_in_last_3s");

			} else if (last_state_id == STATE_ROUND_INGAME_BOMB_PLANTED &&
				   current_state_time > 42.0 &&
				   current_side == 2 &&
				   current_state_id == STATE_ROUND_END_WIN) {
				reporter_trigger("event_victory_in_last_3s");

			} else if (current_state_id == STATE_ROUND_END_WIN && current_side == 1
				|| current_state_id == STATE_ROUND_END_LOSE && current_side == 2) {
				reporter_trigger("event_round_end_attacker");

			} else if (current_state_id == STATE_ROUND_END_WIN && current_side == 2
				|| current_state_id == STATE_ROUND_END_LOSE && current_side == 1) {
				reporter_trigger("event_round_end_defender");
			}

			reset_round_triggers();

		} else if (current_state_id == STATE_ROUND_WIN_ALL_ALIVE) {
			reporter_trigger("event_round_win_all_alive");
			reset_round_triggers();

		} else if (current_state_id == STATE_ROUND_WIN_FIVE_KILL) {
			reporter_trigger("event_round_end_five_kill");
			reset_round_triggers();

		} else if (current_state_id == STATE_ROUND_WIN_ALIVE_1) {
			if (current_side == 1) {
				reporter_trigger("event_round_end_attacker");
			} else if (current_side == 2) {
				reporter_trigger("event_round_end_defender");
			}
			reset_round_triggers();

		} else if (current_state_id == STATE_GAME_END_WIN) {
			reporter_trigger("event_game_end_win");
			reset_round_triggers();

		} else if (current_state_id == STATE_GAME_END_LOSE) {
			reporter_trigger("event_game_end_lose");
			reset_round_triggers();

		} else if (current_state_id == STATE_GAME_END_DRAW) {
			reporter_trigger("event_game_end_draw");
			reset_round_triggers();

		// 下方为无特殊操作的状态事件
		} else if (current_state_id == STATE_GAME_CHOOSE_CHARACTER) {
			reporter_trigger("event_game_choose_character");

		} else if (current_state_id == STATE_GAME_SWITCH_SIDE) {
			reporter_trigger("event_game_switch_side");

		} else if (current_state_id == STATE_ROUND_INGAME) {
			if (current_side == 1) {
				reporter_trigger("event_round_ingame_attacker");
			} else if (current_side == 2) {
				reporter_trigger("event_round_ingame_defender");
			}

		} else if (current_state_id == STATE_ROUND_INGAME_BOMB_PLANTED) {
			reporter_trigger("event_round_ingame_bomb_planted");

		} else {
			reporter_trigger(state_name);
		}

		/* 上面的分支已经使用完旧状态持续时间，现在开始新状态计时。 */
		state_entered_at_ns = now_ns;
		current_state_time = 0.0;

		break;
	}
}
