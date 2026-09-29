// src/reporter-trigger.c - Event trigger and audio playback for Kanami Reporter filter.

#include "reporter-trigger.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>
#include <mmsystem.h>

#include <limits.h>
#include <stdio.h>
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
 * "上半场最后的表演了，稍微调整状态，继续前进吧。.mp3"
 * "公演加时了，香奈美会陪你战斗到最后的。.mp3"
 */

const size_t reporter_voice_table_count =
	sizeof(reporter_voice_table) / sizeof(reporter_voice_table[0]);

/*
 * Windows MP3 player.
 *
 * - Only one announcement plays at a time.
 * - A new announcement closes the previous player.
 * - Playback is asynchronous.
 * - Device access is serialized by an SRW lock.
 */
static SRWLOCK audio_lock = SRWLOCK_INIT;
static MCIDEVICEID audio_device = 0;
static bool audio_shutting_down = false;

static wchar_t *utf8_to_wide(const char *text)
{
	if (!text)
		return NULL;

	int count = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS,
		text, -1, NULL, 0);

	if (count <= 0)
		return NULL;

	wchar_t *result = bmalloc((size_t)count * sizeof(*result));

	if (MultiByteToWideChar(
		    CP_UTF8, MB_ERR_INVALID_CHARS,
		    text, -1, result, count) != count) {
		bfree(result);
		return NULL;
	}

	return result;
}

static void log_mci_error(const char *operation, MCIERROR error)
{
	wchar_t message_w[256] = {0};
	char message_utf8[1024] = {0};

	if (mciGetErrorStringW(error, message_w, 256)) {
		int converted = WideCharToMultiByte(
			CP_UTF8, 0,
			message_w, -1,
			message_utf8, (int)sizeof(message_utf8),
			NULL, NULL);

		if (converted <= 0)
			message_utf8[0] = '\0';
	}

	blog(LOG_WARNING,
	     "[Kanami-Reporter] [AUDIO] %s failed: code=%lu message=%s",
	     operation,
	     (unsigned long)error,
	     message_utf8[0] ? message_utf8 : "Unknown MCI error");
}

/* Caller must hold audio_lock exclusively. */
static bool close_audio_locked(void)
{
	if (!audio_device)
		return true;

	MCIERROR error = mciSendCommandW(
		audio_device,
		MCI_CLOSE,
		MCI_WAIT,
		0);

	if (error) {
		log_mci_error("close", error);
		return false;
	}

	audio_device = 0;
	return true;
}

/*
 * Files live in:
 * <OBS plugin config directory>/voices/<filename>
 *
 * This function returns an allocated UTF-8 path.
 * Caller must bfree() it.
 */
static char *get_audio_path(const char *filename)
{
	if (!filename || !filename[0])
		return NULL;

	char *directory = obs_module_config_path("voices");
	if (!directory) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [AUDIO] Cannot resolve voices directory");
		return NULL;
	}

	os_mkdirs(directory);

	size_t length = strlen(directory) + strlen(filename) + 2;
	char *path = bmalloc(length);

	snprintf(path, length, "%s/%s", directory, filename);

	bfree(directory);
	return path;
}

static bool play_mp3(const char *filename)
{
	char *path = get_audio_path(filename);
	if (!path)
		return false;

	wchar_t *wide_path = utf8_to_wide(path);
	if (!wide_path) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [AUDIO] Invalid UTF-8 path: %s",
		     path);
		bfree(path);
		return false;
	}

	DWORD attributes = GetFileAttributesW(wide_path);

	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [AUDIO] MP3 file not found: %s",
		     path);

		bfree(wide_path);
		bfree(path);
		return false;
	}

	bool started = false;

	AcquireSRWLockExclusive(&audio_lock);

	if (audio_shutting_down)
		goto done;

	/*
	 * Only interrupt the previous announcement after confirming
	 * the requested file exists.
	 */
	if (!close_audio_locked())
		goto done;

	MCI_OPEN_PARMSW open_params = {0};
	open_params.lpstrDeviceType = L"mpegvideo";
	open_params.lpstrElementName = wide_path;

	MCIERROR error = mciSendCommandW(
		0,
		MCI_OPEN,
		MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_WAIT,
		(DWORD_PTR)&open_params);

	if (error) {
		log_mci_error("open", error);
		goto done;
	}

	audio_device = open_params.wDeviceID;

	MCI_PLAY_PARMS play_params = {0};
	play_params.dwFrom = 0;

	/*
	 * No MCI_WAIT here: playback runs asynchronously.
	 * The state machine can continue processing frames.
	 */
	error = mciSendCommandW(
		audio_device,
		MCI_PLAY,
		MCI_FROM,
		(DWORD_PTR)&play_params);

	if (error) {
		log_mci_error("play", error);
		close_audio_locked();
		goto done;
	}

	started = true;

done:
	ReleaseSRWLockExclusive(&audio_lock);

	if (started) {
		blog(LOG_INFO,
		     "[Kanami-Reporter] [AUDIO] Playing: %s",
		     path);
	}

	bfree(wide_path);
	bfree(path);
	return started;
}

void reporter_audio_shutdown(void)
{
	AcquireSRWLockExclusive(&audio_lock);

	audio_shutting_down = true;
	close_audio_locked();

	ReleaseSRWLockExclusive(&audio_lock);
}

void reporter_trigger(const char *event)
{
	if (!event || !event[0])
		return;

	blog(LOG_INFO,
	     "[Kanami-Reporter] [TRIGGER] %s",
	     event);

	for (size_t i = 0; i < reporter_voice_table_count; i++) {
		if (strcmp(reporter_voice_table[i].event, event) != 0)
			continue;

		/* 当前仍然选择该事件的第一条语音。 */
		const char *filename = reporter_voice_table[i].filenames[0];

		if (!filename || !filename[0]) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [AUDIO] 事件尚未配置语音：%s",
			     event);
			return;
		}

		blog(LOG_INFO,
		     "[Kanami-Reporter] 准备播放：%s",
		     filename);

		play_mp3(filename);
		return;
	}

	blog(LOG_INFO,
	     "[Kanami-Reporter] [AUDIO] 未找到事件对应语音：%s",
	     event);
}
