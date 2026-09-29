// src/reporter-trigger.h - Event trigger and audio playback for Kanami Reporter filter.

#pragma once

#include <stddef.h>

#define REPORTER_MAX_VOICES 8

struct reporter_voice_entry {
	const char*  event;
	const char* filenames[REPORTER_MAX_VOICES];
};

extern const struct reporter_voice_entry reporter_voice_table[];
extern const size_t reporter_voice_table_count;

void reporter_trigger(const char* event);

/* 停止播放并释放播放器。插件卸载时调用。 */
void reporter_audio_shutdown(void);
