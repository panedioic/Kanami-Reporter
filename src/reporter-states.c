// src/reporter-states.c - State definitions for Kanami Reporter filter.

#include "reporter-states.h"

#include <stddef.h>

// const char *const reporter_all_states[] = {
// 	"game_choose_character",
// 	"game_start_attacker",
// 	"game_start_defender",
// 	"game_switch_side",
// 	"round_start",
// 	"round_start_last_5s",
// 	"round_ingame",
// 	"round_ingame_bomb_planted",
// 	"round_end_win",
// 	"round_end_lose",
// 	"round_end_five_kill",
// 	"game_end_win",
// 	"game_end_lose",
// 	"game_end_draw",
// };

const char *const state_names[STATE_COUNT] = {
	"game_choose_character",
	"game_start_attacker",
	"game_start_defender",
	"game_switch_side",
	"round_start",
	"round_ingame",
	"round_ingame_bomb_planted",
	"round_end_win",
	"round_end_lose",
	"round_win_five_kill",
	"round_win_all_alive",
	"round_win_alive_1",
	"game_end_win",
	"game_end_lose",
	"game_end_draw",
};
