// src/reporter-trigger.c - Event trigger and audio playback for Kanami Reporter filter.

#include "reporter-trigger.h"

#include <obs-module.h>

#include <string.h>

const struct reporter_voice_entry reporter_voice_table[] = {
	{
		"event_bomber_down",
		{
			"炸弹携带者被击倒了。.mp3"
		},
	},
	{
		"event_alive_players_us_1",
		{
			"这就是最后的希望了！.mp3"
		},
	},
	{
		"event_alive_players_enemy_1",
		{
			"对面的舞台上，只剩下独唱了呢。.mp3"
		},
	},
	{
		"event_round_ingame_last_40s",
		{
			"距离战斗结束还剩40秒。.mp3"
		},
	},
	{
		"event_round_ingame_last_20s",
		{
			"只剩20秒咯。.mp3"
		},
	},
	{
		"event_round_start_last_5s",
		{
			"倒计时！五、四、三、二、一，GO！.mp3"
		},
	},
	{
		"event_game_start_attacker",
		{
			NULL
		},
	},
	{
		"event_game_start_defender",
		{
			NULL
		},
	},
	{
		"event_round_ingame_attacker",
		{
			"保护好炸弹携带者，登上未知的舞台吧！.mp3"
		},
	},
	{
		"event_round_ingame_defender",
		{
			"各位，要守护好阵地哦。.mp3"
		},
	},
	{
		"event_last_round_of_first_half",
		{
			"上半场最后的表演了，稍微调整状态，继续前进吧。.mp3"
		},
	},
	{
		"event_decide_round",
		{
			"只差最后一步，就能走向充满光辉的未来了！.mp3",
			"现在是关键时刻哦.mp3"
		},
	},
	{
		"event_overtime_round",
		{
			"真是势均力敌的对局，就连香奈美也开始心潮澎湃了！.mp3"
		},
	},
	{
		"event_game_start_second_half",
		{
			"后半场的第一首歌可是很重要的。.mp3"
		},
	},
	{
		"event_victory_in_last_3s",
		{
			"好，好险啊，香奈美可一直为你提心吊胆呢！.mp3"
		},
	},
	{
		"event_round_end_attacker",
		{
			"赢了！只要聚集起大家的力量，就一定能改变很多事情。.mp3"
		},
	},
	{
		"event_round_end_defender",
		{
			"各位成功的守护了很多东西呢。.mp3"
		},
	},
	{
		"event_round_win_all_alive",
		{
			"全员留在舞台的身姿，是不是比任何时候都更加耀眼呢？.mp3"
		},
	},
	{
		"event_round_end_five_kill",
		{
			"哇！真是梦寐以求的完美表演呢！.mp3"
		},
	},
	{
		"event_game_end_win",
		{
			"演出完美谢幕，大家辛苦了！.mp3"
		},
	},
	{
		"event_game_end_lose",
		{
			"聚光灯关上了，表演也结束了。.mp3"
		},
	},
	{
		"event_game_end_draw",
		{
			"真是势均力敌的对局，就连香奈美也开始心潮澎湃了！.mp3"
		},
	},
	{
		"event_game_choose_character",
		{
			"选择角色，朝向舞台的最高峰发起进攻吧！.mp3",
			"选择角色，守护最珍贵的事物吧！.mp3"
		},
	},
	{
		"event_game_switch_side",
		{
			"攻守互换.mp3"
		},
	},
	{
		"event_round_ingame",
		{
			NULL
		},
	},
	{
		"event_round_ingame_bomb_planted",
		{
			"炸弹安装完毕，即将为公演增添最璀璨的色彩。.mp3"
		},
	},
};

/* 暂时没有用到的语音，未来加上:
 * "炸弹掉落在了A区。.mp3"
 * "炸弹掉落在了B区。.mp3"
 * "即使面对这样的局面，也战到了最后。.mp3"
 * "你是炸弹携带者，香奈美会祈祷你一切顺利的。.mp3"
 * "演出马上就要开始了，各位，准备好了吗？.mp3"
 */

const size_t reporter_voice_table_count =
	sizeof(reporter_voice_table) / sizeof(reporter_voice_table[0]);

static void play_audio_placeholder(const char *filename)
{
	/* TODO: 后续接入实际MP3播放。 */
	blog(LOG_INFO,
	     "[Kanami-Reporter] 准备播放：%s",
	     filename);
}

void reporter_trigger(const char *event)
{
	if (!event)
		return;

	blog(LOG_INFO,
	     "[Kanami-Reporter] [TRIGGER] %s",
	     event);

	for (size_t i = 0; i < reporter_voice_table_count; i++) {
		if (strcmp(reporter_voice_table[i].event, event) != 0)
			continue;

		const char *filename = reporter_voice_table[i].filenames[0];

		if (filename && filename[0]) {
			play_audio_placeholder(filename);
		} else {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [AUDIO] 事件尚未配置语音：%s",
			     event);
		}

		return;
	}

	blog(LOG_INFO,
	     "[Kanami-Reporter] [AUDIO] 未找到事件对应语音：%s",
	     event);
}
