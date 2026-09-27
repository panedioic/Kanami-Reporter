/*
Kanami-Reporter
Copyright (C) 2026 Yiyan me@y1yan.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "voice-filter.h"

#include <graphics/graphics.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>
#include <process.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Recognition coordinates use the source resolution directly.
 * One frame is captured every 100 ms (10 Hz).
 */
#define FRAME_W 1920
#define FRAME_H 1080
#define FRAME_BYTES ((size_t)FRAME_W * FRAME_H * 4)
#define CAPTURE_NS 100000000ULL

/* Area limit for a single event template, not for the whole frame. */
#define MAX_TEMPLATE_PIXELS 262144

enum cue_id {
	CUE_MATCHING,
	CUE_CONFIRM,
	CUE_SELECT,
	CUE_LOADING,
	CUE_PREP_ATTACK,
	CUE_PREP_DEFENSE,
	CUE_LIVE,
	CUE_WIN,
	CUE_LOSE,
	CUE_PLANTED,
	CUE_DEFUSED,
	CUE_EXPLODED,
	CUE_OVERTIME,
	CUE_KILL,
	CUE_ASSIST,
	CUE_COUNT
};

static const char *cue_names[CUE_COUNT] = {
	"matching",
	"confirmation",
	"character_selection",
	"loading",
	"prepare_attack",
	"prepare_defense",
	"round_live",
	"round_won",
	"round_lost",
	"bomb_planted",
	"bomb_defused",
	"bomb_exploded",
	"overtime",
	"self_kill",
	"self_assist",
};

enum phase {
	PH_UNKNOWN,
	PH_MATCHING,
	PH_CONFIRM,
	PH_SELECT,
	PH_LOADING,
	PH_PREP,
	PH_LIVE,
	PH_ENDED
};

static const char *phase_names[] = {
	"unknown",
	"matching",
	"confirmation",
	"character_selection",
	"loading",
	"round_preparing",
	"round_live",
	"round_ended",
};

enum bomb_state {
	BOMB_UNKNOWN,
	BOMB_NONE,
	BOMB_PLANTED,
	BOMB_DEFUSED,
	BOMB_EXPLODED
};

struct roi {
	int x;
	int y;
	int w;
	int h;
};

struct template_data {
	struct roi roi;
	uint8_t *pixels;
	double mean;
	double energy;
};

struct frame_data {
	uint8_t *bgra;
	uint64_t timestamp;
	bool recognize;
	double threshold;
};

struct reporter_state {
	enum phase phase;
	enum bomb_state bomb;
	unsigned int round;
	int side; /* 0 unknown, 1 attack, 2 defense */
	bool overtime;

	uint32_t emitted;
	uint8_t history[CUE_COUNT];
	uint8_t misses[CUE_COUNT];
	bool latched[CUE_COUNT];

	uint64_t last_frame;
	uint64_t phase_since;
	uint64_t bomb_since;

	bool phase_reference_valid;
	bool phase_reference_logged;
	bool bomb_reference_logged;
};

struct kanami_reporter_data {
	obs_source_t *source;

	CRITICAL_SECTION lock;
	HANDLE wake;
	HANDLE worker;
	bool stop;

	bool enabled;
	double threshold;
	int selected_cue;
	struct roi selected_roi;

	int train_request;
	struct roi train_roi;
	uint64_t train_after;

	bool preview_requested;
	struct roi preview_roi;
	uint64_t preview_after;

	struct frame_data *pending;
	uint64_t dropped;

	uint64_t last_capture;
	gs_texrender_t *capture;
	gs_stagesurf_t *stage;

	char *template_dir;
};

/* ---------- General helpers ---------- */

static void frame_free(struct frame_data *frame)
{
	if (!frame)
		return;

	bfree(frame->bgra);
	bfree(frame);
}

static int setting_int(obs_data_t *settings, const char *name,
		       int minimum, int maximum)
{
	long long value = obs_data_get_int(settings, name);

	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;

	return (int)value;
}

/*
 * Valid training ROI: inside the frame and small enough
 * to keep matching cost bounded.
 */
static bool valid_roi(struct roi roi)
{
	return roi.x >= 0 && roi.y >= 0 &&
	       roi.w >= 4 && roi.h >= 4 &&
	       roi.x + roi.w <= FRAME_W &&
	       roi.y + roi.h <= FRAME_H &&
	       roi.w * roi.h <= MAX_TEMPLATE_PIXELS;
}

/* ---------- Template file (KRT) ---------- */

static char *template_path(const char *dir, int cue)
{
	size_t length = strlen(dir) + strlen(cue_names[cue]) + 16;
	char *path = bmalloc(length);

	snprintf(path, length, "%s/%s.krt", dir, cue_names[cue]);
	return path;
}

static void template_free(struct template_data *model)
{
	bfree(model->pixels);
	memset(model, 0, sizeof(*model));
}

static bool template_statistics(struct template_data *model)
{
	int count = model->roi.w * model->roi.h;
	double sum = 0.0;
	double squared = 0.0;

	for (int i = 0; i < count; i++) {
		double value = model->pixels[i];
		sum += value;
		squared += value * value;
	}

	model->mean = sum / count;
	model->energy = squared - sum * sum / count;

	/* Reject almost uniform regions: they cannot be matched reliably. */
	return model->energy / count >= 16.0;
}

/*
 * KRT is a local template format.
 * Header: magic, version, frame width/height, ROI x/y/w/h.
 * Payload: grayscale ROI pixels.
 */
static bool template_save(const char *path,
			  const struct template_data *model)
{
	uint32_t header[8] = {
		0x3154524bU,
		1,
		FRAME_W,
		FRAME_H,
		(uint32_t)model->roi.x,
		(uint32_t)model->roi.y,
		(uint32_t)model->roi.w,
		(uint32_t)model->roi.h,
	};

	FILE *file = os_fopen(path, "wb");
	if (!file)
		return false;

	size_t count = (size_t)model->roi.w * (size_t)model->roi.h;

	bool ok = fwrite(header, sizeof(header), 1, file) == 1;
	if (ok)
		ok = fwrite(model->pixels, 1, count, file) == count;

	if (fclose(file) != 0)
		ok = false;

	return ok;
}

static bool template_load(const char *path, struct template_data *model)
{
	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	uint32_t header[8] = {0};
	bool ok = fread(header, sizeof(header), 1, file) == 1;

	if (!ok ||
	    header[0] != 0x3154524bU || header[1] != 1 ||
	    header[2] != FRAME_W || header[3] != FRAME_H ||
	    header[4] >= FRAME_W || header[5] >= FRAME_H ||
	    header[6] > FRAME_W || header[7] > FRAME_H) {
		fclose(file);
		return false;
	}

	model->roi = (struct roi){
		(int)header[4], (int)header[5],
		(int)header[6], (int)header[7]
	};

	if (!valid_roi(model->roi)) {
		fclose(file);
		return false;
	}

	size_t count = (size_t)model->roi.w * (size_t)model->roi.h;
	model->pixels = bmalloc(count);

	ok = fread(model->pixels, 1, count, file) == count;
	fclose(file);

	if (!ok || !template_statistics(model)) {
		template_free(model);
		return false;
	}

	return true;
}

/* ---------- Per-event ROI settings ---------- */

static void roi_setting_key(char *buffer, size_t size,
			    int cue, const char *field)
{
	snprintf(buffer, size, "event_roi_%s_%s", cue_names[cue], field);
}

static void remember_event_roi(obs_data_t *settings, int cue)
{
	if (cue < 0 || cue >= CUE_COUNT)
		return;

	static const char *fields[] = {
		"roi_x", "roi_y", "roi_width", "roi_height"
	};

	char key[128];

	for (size_t i = 0; i < 4; i++) {
		roi_setting_key(key, sizeof(key), cue, fields[i]);
		obs_data_set_int(settings, key,
				 obs_data_get_int(settings, fields[i]));
	}
}

static void restore_event_roi(obs_data_t *settings, int cue)
{
	if (cue < 0 || cue >= CUE_COUNT)
		return;

	static const char *fields[] = {
		"roi_x", "roi_y", "roi_width", "roi_height"
	};

	char key[128];
	bool has_saved_settings = true;

	for (size_t i = 0; i < 4; i++) {
		roi_setting_key(key, sizeof(key), cue, fields[i]);

		if (!obs_data_has_user_value(settings, key)) {
			has_saved_settings = false;
			break;
		}
	}

	if (has_saved_settings) {
		for (size_t i = 0; i < 4; i++) {
			roi_setting_key(key, sizeof(key), cue, fields[i]);
			obs_data_set_int(settings, fields[i],
					 obs_data_get_int(settings, key));
		}
		return;
	}

	/*
	 * Migrate existing templates:
	 * if this event has no UI settings yet, read its stored ROI.
	 */
	struct roi restored = {840, 170, 240, 60};
	struct template_data model = {0};

	char *directory = obs_module_config_path("templates");

	if (directory) {
		char *path = template_path(directory, cue);

		if (template_load(path, &model)) {
			restored = model.roi;
			template_free(&model);
		}

		bfree(path);
		bfree(directory);
	}

	obs_data_set_int(settings, "roi_x", restored.x);
	obs_data_set_int(settings, "roi_y", restored.y);
	obs_data_set_int(settings, "roi_width", restored.w);
	obs_data_set_int(settings, "roi_height", restored.h);

	remember_event_roi(settings, cue);
}

static bool template_event_changed(obs_properties_t *props,
				   obs_property_t *property,
				   obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	int next = setting_int(
		settings, "template_event", 0, CUE_COUNT - 1);

	/*
	 * OBS may invoke modified callbacks when opening the dialog.
	 * Do not replace current edits when the selection did not change.
	 */
	if (!obs_data_has_user_value(settings, "roi_display_event")) {
		restore_event_roi(settings, next);
		obs_data_set_int(settings, "roi_display_event", next);
		return true;
	}

	int previous = setting_int(
		settings, "roi_display_event", 0, CUE_COUNT - 1);

	if (previous == next)
		return false;

	/* The visible coordinates still belong to the previous event. */
	remember_event_roi(settings, previous);
	restore_event_roi(settings, next);
	obs_data_set_int(settings, "roi_display_event", next);

	/* Ask OBS to refresh the property controls from settings. */
	return true;
}

/* ---------- Image comparison ---------- */

static void to_grayscale(const uint8_t *bgra, uint8_t *gray)
{
	for (size_t i = 0; i < (size_t)FRAME_W * FRAME_H; i++) {
		const uint8_t *pixel = bgra + i * 4;

		gray[i] = (uint8_t)(
			(29U * pixel[0] +
			 150U * pixel[1] +
			 77U * pixel[2]) >> 8);
	}
}

/*
 * Normalized cross correlation.
 * Compare at the stored ROI and allow a one-pixel positional offset.
 */
static double template_score(const struct template_data *model,
			     const uint8_t *gray)
{
	static const int offsets[5][2] = {
		{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}
	};

	if (!model->pixels)
		return -1.0;

	const struct roi roi = model->roi;
	const int count = roi.w * roi.h;
	double best = -1.0;

	for (int offset = 0; offset < 5; offset++) {
		int x0 = roi.x + offsets[offset][0];
		int y0 = roi.y + offsets[offset][1];

		if (x0 < 0 || y0 < 0 ||
		    x0 + roi.w > FRAME_W ||
		    y0 + roi.h > FRAME_H)
			continue;

		double sum = 0.0;
		double squared = 0.0;
		double product = 0.0;
		int index = 0;

		for (int y = 0; y < roi.h; y++) {
			const uint8_t *row =
				gray + (size_t)(y0 + y) * FRAME_W + x0;

			for (int x = 0; x < roi.w; x++, index++) {
				double value = row[x];

				sum += value;
				squared += value * value;
				product += value *
					   (model->pixels[index] - model->mean);
			}
		}

		double energy = squared - sum * sum / count;
		if (energy < 1.0)
			continue;

		double score = product / sqrt(energy * model->energy);
		if (score > best)
			best = score;
	}

	return best;
}

/* ---------- State machine: worker thread only ---------- */

static void clear_detection_history(struct reporter_state *state)
{
	memset(state->history, 0, sizeof(state->history));
	memset(state->misses, 0, sizeof(state->misses));
	memset(state->latched, 0, sizeof(state->latched));
}

static void begin_round(struct reporter_state *state)
{
	state->round++;
	state->bomb = BOMB_NONE;
	state->bomb_since = 0;
	state->bomb_reference_logged = false;
	state->emitted = 0;
}

static void change_phase(struct reporter_state *state,
			 enum phase next, uint64_t timestamp)
{
	if (state->phase == next)
		return;

	enum phase previous = state->phase;

	if (next == PH_PREP) {
		begin_round(state);
	} else if (next == PH_LIVE &&
		   previous != PH_PREP &&
		   previous != PH_LIVE) {
		/*
		 * Joined during a round. This is a local identifier,
		 * not the actual round number from the scoreboard.
		 */
		begin_round(state);
		state->bomb = BOMB_UNKNOWN;
	}

	state->phase = next;
	state->phase_since = timestamp;
	state->phase_reference_logged = false;

	/*
	 * Only estimate phase duration after a plausible observed
	 * boundary. Initial synchronization has no reliable start time.
	 */
	state->phase_reference_valid =
		(next == PH_PREP &&
		 (previous == PH_ENDED || previous == PH_LOADING)) ||
		(next == PH_LIVE && previous == PH_PREP) ||
		(next == PH_ENDED &&
		 (previous == PH_LIVE || previous == PH_PREP));

	blog(LOG_INFO,
	     "[Kanami-Reporter] [%s] %s -> %s local_round=%u",
	     previous == PH_UNKNOWN ? "SYNC" : "STATE",
	     phase_names[previous], phase_names[next], state->round);
}

static void emit_once(struct reporter_state *state, int cue)
{
	uint32_t bit = 1U << (unsigned int)cue;

	if (state->emitted & bit)
		return;

	state->emitted |= bit;

	blog(LOG_INFO,
	     "[Kanami-Reporter] [EVENT] %s local_round=%u",
	     cue_names[cue], state->round);
}

static void reference_tick(struct reporter_state *state, uint64_t now)
{
	unsigned int duration = 0;

	if (state->phase == PH_PREP)
		duration = 45;
	else if (state->phase == PH_LIVE && state->bomb == BOMB_NONE)
		duration = 115;
	else if (state->phase == PH_ENDED)
		duration = 8;

	if (duration && state->phase_reference_valid &&
	    !state->phase_reference_logged &&
	    now - state->phase_since >= (uint64_t)duration * 1000000000ULL) {
		state->phase_reference_logged = true;

		blog(LOG_INFO,
		     "[Kanami-Reporter] [REFERENCE] expected_timeout "
		     "phase=%s awaiting_visual_confirmation",
		     phase_names[state->phase]);
	}

	if (state->phase == PH_LIVE &&
	    state->bomb == BOMB_PLANTED &&
	    state->bomb_since &&
	    !state->bomb_reference_logged &&
	    now - state->bomb_since >= 45000000000ULL) {
		state->bomb_reference_logged = true;

		blog(LOG_INFO,
		     "[Kanami-Reporter] [REFERENCE] bomb_estimate_elapsed "
		     "awaiting_visual_confirmation");
	}
}

static void update_state(struct reporter_state *state,
			 const double scores[CUE_COUNT],
			 double threshold, uint64_t now)
{
	bool hit[CUE_COUNT] = {false};
	bool stable[CUE_COUNT] = {false};
	bool edge[CUE_COUNT] = {false};

	/* Do not confirm using observations separated by a long gap. */
	if (state->last_frame && now - state->last_frame > 500000000ULL)
		clear_detection_history(state);

	state->last_frame = now;

	for (int i = 0; i < CUE_COUNT; i++)
		hit[i] = scores[i] >= threshold;

	/*
	 * These cues are mutually exclusive.
	 * Ambiguous matches do not become evidence.
	 */
	if (hit[CUE_WIN] && hit[CUE_LOSE]) {
		hit[CUE_WIN] = false;
		hit[CUE_LOSE] = false;
	}

	if (hit[CUE_PREP_ATTACK] && hit[CUE_PREP_DEFENSE]) {
		hit[CUE_PREP_ATTACK] = false;
		hit[CUE_PREP_DEFENSE] = false;
	}

	for (int i = 0; i < CUE_COUNT; i++) {
		state->history[i] =
			(uint8_t)(((unsigned int)state->history[i] << 1 |
				   (hit[i] ? 1U : 0U)) & 7U);

		unsigned int bits = state->history[i];
		unsigned int count =
			(bits & 1U) + ((bits >> 1) & 1U) + ((bits >> 2) & 1U);

		/* Require a current hit and at least two hits in three frames. */
		stable[i] = hit[i] && count >= 2;

		if (hit[i]) {
			state->misses[i] = 0;
		} else if (state->misses[i] < 3) {
			state->misses[i]++;
		}

		if (state->misses[i] >= 3)
			state->latched[i] = false;

		edge[i] = stable[i] && !state->latched[i];

		if (stable[i])
			state->latched[i] = true;
	}

	/* Recognize pre-match screens only from their own strong cues. */
	int menu = -1;
	for (int i = CUE_MATCHING; i <= CUE_LOADING; i++) {
		if (stable[i] && (menu < 0 || scores[i] > scores[menu]))
			menu = i;
	}

	if (menu >= 0) {
		static const enum phase menu_phases[] = {
			PH_MATCHING, PH_CONFIRM, PH_SELECT, PH_LOADING
		};

		enum phase next = menu_phases[menu];
		if (state->phase != next) {
			if (state->phase >= PH_PREP) {
				state->round = 0;
				state->side = 0;
				state->overtime = false;
				state->bomb = BOMB_UNKNOWN;
				state->emitted = 0;
			}

			change_phase(state, next, now);
		}
		return;
	}

	if (stable[CUE_OVERTIME] && !state->overtime) {
		state->overtime = true;
		blog(LOG_INFO,
		     "[Kanami-Reporter] [STATE] competition=overtime");
	}

	/*
	 * Round result takes precedence over remaining combat cues.
	 * Bomb outcome and round result can both be represented.
	 */
	if (stable[CUE_WIN] || stable[CUE_LOSE]) {
		bool initial = state->phase == PH_UNKNOWN;
		bool newly_ended = state->phase != PH_ENDED;

		change_phase(state, PH_ENDED, now);

		if (stable[CUE_DEFUSED])
			state->bomb = BOMB_DEFUSED;
		else if (stable[CUE_EXPLODED])
			state->bomb = BOMB_EXPLODED;

		if (newly_ended && !initial)
			emit_once(state, stable[CUE_WIN] ? CUE_WIN : CUE_LOSE);

		if (newly_ended && initial) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [SYNC] result=%s",
			     stable[CUE_WIN] ? "won" : "lost");
		}

		/* Reasons may become readable after the main result. */
		if (stable[CUE_DEFUSED])
			emit_once(state, CUE_DEFUSED);
		else if (stable[CUE_EXPLODED])
			emit_once(state, CUE_EXPLODED);

		reference_tick(state, now);
		return;
	}

	int prepare_side = stable[CUE_PREP_ATTACK] ? 1 :
			   stable[CUE_PREP_DEFENSE] ? 2 : 0;

	if (prepare_side) {
		/*
		 * Do not rewind an active round because a task label
		 * briefly reappears.
		 */
		if (state->phase != PH_LIVE) {
			change_phase(state, PH_PREP, now);
			state->side = prepare_side;
		}
	} else if (stable[CUE_LIVE] && state->phase != PH_ENDED) {
		bool from_preparation = state->phase == PH_PREP;

		change_phase(state, PH_LIVE, now);

		if (from_preparation)
			emit_once(state, CUE_LIVE);
	}

	if (stable[CUE_PLANTED] &&
	    state->phase != PH_ENDED &&
	    state->bomb != BOMB_PLANTED &&
	    state->bomb != BOMB_DEFUSED &&
	    state->bomb != BOMB_EXPLODED) {
		bool observed_during_round =
			state->phase == PH_LIVE && state->bomb == BOMB_NONE;

		change_phase(state, PH_LIVE, now);
		state->bomb = BOMB_PLANTED;
		state->bomb_since = observed_during_round ? now : 0;

		if (observed_during_round) {
			emit_once(state, CUE_PLANTED);
		} else {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [SYNC] bomb=planted "
			     "remaining_time=unknown");
		}
	}

	if (state->phase == PH_LIVE) {
		if (stable[CUE_DEFUSED]) {
			state->bomb = BOMB_DEFUSED;
			emit_once(state, CUE_DEFUSED);
		} else if (stable[CUE_EXPLODED]) {
			state->bomb = BOMB_EXPLODED;
			emit_once(state, CUE_EXPLODED);
		}

		/* These events can occur more than once per round. */
		if (edge[CUE_KILL]) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [EVENT] self_kill local_round=%u",
			     state->round);
		}

		if (edge[CUE_ASSIST]) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [EVENT] self_assist local_round=%u",
			     state->round);
		}
	}

	reference_tick(state, now);
}

/* ---------- ROI preview (worker thread) ---------- */

static void bmp_u16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
}

static void bmp_u32(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
	dst[2] = (uint8_t)(value >> 16);
	dst[3] = (uint8_t)(value >> 24);
}

static void save_roi_preview(const struct frame_data *frame, struct roi roi)
{
	const size_t row_bytes = (size_t)FRAME_W * 4;
	const uint32_t image_bytes = (uint32_t)FRAME_BYTES;

	/* Draw on a copy so the recognition input stays untouched. */
	uint8_t *image = bmalloc(FRAME_BYTES);
	memcpy(image, frame->bgra, FRAME_BYTES);

	/*
	 * Draw only where the border actually lies inside the frame.
	 * An out-of-range ROI is not disguised as a clipped rectangle.
	 */
	const int right = roi.x + roi.w - 1;
	const int bottom = roi.y + roi.h - 1;
	const int thickness = 3;

	for (int y = 0; y < FRAME_H; y++) {
		for (int x = 0; x < FRAME_W; x++) {
			bool horizontal =
				x >= roi.x && x <= right &&
				((y >= roi.y && y < roi.y + thickness) ||
				 (y <= bottom && y > bottom - thickness));

			bool vertical =
				y >= roi.y && y <= bottom &&
				((x >= roi.x && x < roi.x + thickness) ||
				 (x <= right && x > right - thickness));

			if (horizontal || vertical) {
				uint8_t *pixel =
					image + ((size_t)y * FRAME_W +
						 (size_t)x) * 4;

				pixel[0] = 0;
				pixel[1] = 0;
				pixel[2] = 255;
				pixel[3] = 255;
			}
		}
	}

	uint8_t header[54] = {0};
	header[0] = 'B';
	header[1] = 'M';

	bmp_u32(header + 2, 54U + image_bytes);
	bmp_u32(header + 10, 54);
	bmp_u32(header + 14, 40);
	bmp_u32(header + 18, FRAME_W);
	bmp_u32(header + 22, (uint32_t)(-(int32_t)FRAME_H));
	bmp_u16(header + 26, 1);
	bmp_u16(header + 28, 32);
	bmp_u32(header + 34, image_bytes);

	/*
	 * Always overwrite the same file to avoid accumulating
	 * multi-megabyte previews in the plugin config directory.
	 */
	char filename[96];
	snprintf(filename, sizeof(filename), "roi-preview.bmp");

	char *path = obs_module_config_path(filename);
	if (!path) {
		bfree(image);
		return;
	}

	FILE *file = os_fopen(path, "wb");
	bool ok = false;

	if (file) {
		ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);

		for (size_t y = 0; ok && y < FRAME_H; y++) {
			ok = fwrite(image + y * row_bytes, 1, row_bytes, file)
			     == row_bytes;
		}

		if (fclose(file) != 0)
			ok = false;
	}

	blog(ok ? LOG_INFO : LOG_WARNING,
	     "[Kanami-Reporter] [PREVIEW] saved=%s roi=%d,%d,%d,%d "
	     "valid_for_training=%s path=%s",
	     ok ? "true" : "false",
	     roi.x, roi.y, roi.w, roi.h,
	     valid_roi(roi) ? "true" : "false", path);

	bfree(path);
	bfree(image);
}

/* ---------- Template training (worker thread) ---------- */

static void train_template(struct kanami_reporter_data *data,
			   struct template_data models[CUE_COUNT],
			   int cue, struct roi roi, const uint8_t *gray)
{
	if (!valid_roi(roi)) {
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

	if (!template_statistics(&model)) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] Template rejected: almost uniform ROI.");
		template_free(&model);
		return;
	}

	char *path = template_path(data->template_dir, cue);
	bool saved = template_save(path, &model);

	if (saved) {
		template_free(&models[cue]);
		models[cue] = model;

		blog(LOG_INFO,
		     "[Kanami-Reporter] [TEMPLATE] saved=%s roi=%d,%d,%d,%d "
		     "path=%s",
		     cue_names[cue], roi.x, roi.y, roi.w, roi.h, path);
	} else {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] Cannot save template: %s", path);
		template_free(&model);
	}

	bfree(path);
}

/* ---------- Worker thread ---------- */

static unsigned int __stdcall reporter_worker(void *argument)
{
	struct kanami_reporter_data *data = argument;
	struct template_data models[CUE_COUNT] = {0};
	struct reporter_state state = {0};
	uint8_t *gray = bmalloc((size_t)FRAME_W * FRAME_H);
	int loaded = 0;

	for (int i = 0; i < CUE_COUNT; i++) {
		char *path = template_path(data->template_dir, i);

		if (template_load(path, &models[i]))
			loaded++;

		bfree(path);
	}

	blog(LOG_INFO, "[Kanami-Reporter] Loaded %d templates from %s",
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
		struct roi train_roi = {0};

		if (frame && data->train_request >= 0 &&
		    frame->timestamp >= data->train_after) {
			train = data->train_request;
			train_roi = data->train_roi;
			data->train_request = -1;
		}

		bool preview = false;
		struct roi preview_roi = {0};

		if (frame && data->preview_requested &&
		    frame->timestamp >= data->preview_after) {
			preview = true;
			preview_roi = data->preview_roi;
			data->preview_requested = false;
		}

		uint64_t dropped = data->dropped;
		LeaveCriticalSection(&data->lock);

		if (stop) {
			frame_free(frame);
			break;
		}

		if (!frame)
			continue;

		uint64_t started = os_gettime_ns();

		/* Ignore old frames instead of recognizing delayed events. */
		if (started - frame->timestamp > 500000000ULL) {
			frame_free(frame);
			continue;
		}

		if (preview)
			save_roi_preview(frame, preview_roi);

		to_grayscale(frame->bgra, gray);

		if (train >= 0) {
			train_template(data, models, train, train_roi, gray);

			/* The template set changed: restart recognition. */
			memset(&state, 0, sizeof(state));
			was_enabled = false;
		} else if (frame->recognize) {
			if (!was_enabled)
				memset(&state, 0, sizeof(state));

			double scores[CUE_COUNT];
			for (int i = 0; i < CUE_COUNT; i++)
				scores[i] = template_score(&models[i], gray);

			update_state(&state, scores, frame->threshold,
				     frame->timestamp);
			was_enabled = true;
		} else {
			was_enabled = false;
		}

		work_ns += os_gettime_ns() - started;
		processed++;

		uint64_t now = os_gettime_ns();
		if (now - report_since >= 120000000000ULL) {
			blog(LOG_INFO,
			     "[Kanami-Reporter] [PERF] processed=%llu "
			     "dropped=%llu avg_worker_ms=%.2f",
			     (unsigned long long)processed,
			     (unsigned long long)(dropped - previous_dropped),
			     processed ? (double)work_ns /
					 (double)processed / 1000000.0 : 0.0);

			processed = 0;
			work_ns = 0;
			report_since = now;
			previous_dropped = dropped;
		}

		frame_free(frame);
	}

	for (int i = 0; i < CUE_COUNT; i++)
		template_free(&models[i]);

	bfree(gray);
	return 0;
}

/* ---------- OBS properties and lifecycle ---------- */

static const char *reporter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Kanami Reporter";
}

static void reporter_update(void *private_data, obs_data_t *settings)
{
	struct kanami_reporter_data *data = private_data;

	int selected = setting_int(
		settings, "template_event", 0, CUE_COUNT - 1);

	if (!obs_data_has_user_value(settings, "roi_display_event")) {
		restore_event_roi(settings, selected);
		obs_data_set_int(settings, "roi_display_event", selected);
	} else {
		int displayed = setting_int(
			settings, "roi_display_event", 0, CUE_COUNT - 1);

		if (displayed != selected) {
			remember_event_roi(settings, displayed);
			restore_event_roi(settings, selected);
			obs_data_set_int(settings, "roi_display_event", selected);
		}
	}

	struct roi selected_roi = {
		setting_int(settings, "roi_x", 0, FRAME_W - 1),
		setting_int(settings, "roi_y", 0, FRAME_H - 1),
		setting_int(settings, "roi_width", 4, FRAME_W),
		setting_int(settings, "roi_height", 4, FRAME_H),
	};

	/* Keep displayed settings and the worker's coordinates consistent. */
	obs_data_set_int(settings, "roi_x", selected_roi.x);
	obs_data_set_int(settings, "roi_y", selected_roi.y);
	obs_data_set_int(settings, "roi_width", selected_roi.w);
	obs_data_set_int(settings, "roi_height", selected_roi.h);
	remember_event_roi(settings, selected);

	bool enabled = obs_data_get_bool(settings, "recognition_enabled");
	double threshold = obs_data_get_double(settings, "match_threshold");

	if (!(threshold >= 0.50 && threshold <= 0.999))
		threshold = 0.90;

	EnterCriticalSection(&data->lock);

	data->enabled = enabled;
	data->threshold = threshold;
	data->selected_cue = selected;
	data->selected_roi = selected_roi;

	LeaveCriticalSection(&data->lock);
}

static void reporter_defaults(obs_data_t *settings)
{
	/* Start in calibration mode to avoid accidental recognition. */
	obs_data_set_default_bool(settings, "recognition_enabled", false);
	obs_data_set_default_double(settings, "match_threshold", 0.90);
	obs_data_set_default_int(settings, "template_event", CUE_WIN);
	obs_data_set_default_int(settings, "roi_x", 840);
	obs_data_set_default_int(settings, "roi_y", 170);
	obs_data_set_default_int(settings, "roi_width", 240);
	obs_data_set_default_int(settings, "roi_height", 60);
}

static bool request_template(obs_properties_t *props,
			     obs_property_t *property, void *private_data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = private_data;
	if (!data)
		return false;

	EnterCriticalSection(&data->lock);
	data->train_request = data->selected_cue;
	data->train_roi = data->selected_roi;
	data->train_after = os_gettime_ns();
	LeaveCriticalSection(&data->lock);

	blog(LOG_INFO,
	     "[Kanami-Reporter] Template capture requested. "
	     "Keep the source visible.");

	return false;
}

static bool request_preview(obs_properties_t *props,
			    obs_property_t *property, void *private_data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	struct kanami_reporter_data *data = private_data;
	if (!data)
		return false;

	EnterCriticalSection(&data->lock);
	data->preview_requested = true;
	data->preview_roi = data->selected_roi;
	data->preview_after = os_gettime_ns();
	LeaveCriticalSection(&data->lock);

	blog(LOG_INFO,
	     "[Kanami-Reporter] ROI preview requested. "
	     "Keep the source visible.");

	return false;
}

static obs_properties_t *reporter_properties(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(
		props, "recognition_enabled", "Enable recognition");

	obs_properties_add_float_slider(
		props, "match_threshold", "Template match threshold",
		0.50, 0.999, 0.001);

	obs_property_t *events = obs_properties_add_list(
		props, "template_event", "Template event",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	for (int i = 0; i < CUE_COUNT; i++)
		obs_property_list_add_int(events, cue_names[i], i);

	obs_property_set_modified_callback(events, template_event_changed);

	obs_properties_add_int(
		props, "roi_x", "Template X (1920x1080)", 0, FRAME_W - 1, 1);
	obs_properties_add_int(
		props, "roi_y", "Template Y (1920x1080)", 0, FRAME_H - 1, 1);
	obs_properties_add_int(
		props, "roi_width", "Template width", 4, FRAME_W, 1);
	obs_properties_add_int(
		props, "roi_height", "Template height", 4, FRAME_H, 1);

	obs_properties_add_button(
		props, "preview_roi", "Save ROI preview (red rectangle)",
		request_preview);

	obs_properties_add_button(
		props, "save_template", "Save / replace event template",
		request_template);

	obs_properties_add_text(
		props, "help",
		"Coordinates use a 1920x1080 image. "
		"Save an ROI preview to check the position before saving a "
		"template. Each event keeps its own ROI. "
		"Saving replaces that event's template and resets "
		"recognition state.",
		OBS_TEXT_INFO);

	return props;
}

static void *reporter_create(obs_data_t *settings, obs_source_t *source)
{
	struct kanami_reporter_data *data = bzalloc(sizeof(*data));

	data->source = source;
	data->train_request = -1;

	InitializeCriticalSection(&data->lock);
	data->wake = CreateEventW(NULL, FALSE, FALSE, NULL);

	char *directory = obs_module_config_path("templates");
	data->template_dir = directory;

	if (directory)
		os_mkdirs(directory);

	obs_enter_graphics();
	data->capture = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	data->stage = gs_stagesurface_create(FRAME_W, FRAME_H, GS_BGRA);
	obs_leave_graphics();

	reporter_update(data, settings);

	if (data->wake && data->template_dir &&
	    data->capture && data->stage) {
		uintptr_t handle =
			_beginthreadex(NULL, 0, reporter_worker, data, 0, NULL);

		data->worker = (HANDLE)handle;
	}

	if (!data->worker) {
		obs_enter_graphics();
		if (data->stage)
			gs_stagesurface_destroy(data->stage);
		if (data->capture)
			gs_texrender_destroy(data->capture);
		obs_leave_graphics();

		if (data->wake)
			CloseHandle(data->wake);

		DeleteCriticalSection(&data->lock);
		bfree(data->template_dir);
		bfree(data);
		return NULL;
	}

	return data;
}

static void reporter_destroy(void *private_data)
{
	struct kanami_reporter_data *data = private_data;

	if (!data)
		return;

	EnterCriticalSection(&data->lock);
	data->stop = true;
	LeaveCriticalSection(&data->lock);

	SetEvent(data->wake);
	WaitForSingleObject(data->worker, INFINITE);

	CloseHandle(data->worker);
	CloseHandle(data->wake);
	frame_free(data->pending);

	obs_enter_graphics();
	gs_stagesurface_destroy(data->stage);
	gs_texrender_destroy(data->capture);
	obs_leave_graphics();

	DeleteCriticalSection(&data->lock);
	bfree(data->template_dir);
	bfree(data);
}

/* ---------- Graphics thread: one frame every 100 ms ---------- */

static void capture_frame(struct kanami_reporter_data *data, uint64_t now,
			  bool enabled, double threshold)
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
	 * Render a synchronous parent source directly to avoid
	 * re-entering its own filter chain.
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

	gs_stage_texture(data->stage, gs_texrender_get_texture(data->capture));

	uint8_t *pixels = NULL;
	uint32_t stride = 0;

	if (!gs_stagesurface_map(data->stage, &pixels, &stride))
		return;

	struct frame_data *frame = bzalloc(sizeof(*frame));
	frame->bgra = bmalloc(FRAME_BYTES);
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

	frame_free(old);
	SetEvent(data->wake);
}

static void reporter_render(void *private_data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct kanami_reporter_data *data = private_data;
	uint64_t now = os_gettime_ns();

	EnterCriticalSection(&data->lock);
	bool enabled = data->enabled;
	double threshold = data->threshold;
	LeaveCriticalSection(&data->lock);

	/* Capture at 10 Hz, including while calibrating templates. */
	if (!data->last_capture || now - data->last_capture >= CAPTURE_NS) {
		data->last_capture = now;
		capture_frame(data, now, enabled, threshold);
	}

	obs_source_skip_video_filter(data->source);
}

struct obs_source_info kanami_reporter_filter = {
	.id = "kanami_reporter_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,

	.get_name = reporter_name,
	.create = reporter_create,
	.destroy = reporter_destroy,
	.get_defaults = reporter_defaults,
	.get_properties = reporter_properties,
	.update = reporter_update,
	.video_render = reporter_render,
};