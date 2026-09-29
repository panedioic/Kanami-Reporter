// src/reporter-template.c - Template recognition and management for Kanami Reporter filter.

#include "reporter-template.h"
#include "reporter-states.h"

#include <util/bmem.h>
#include <util/platform.h>
#include <obs-module.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

bool reporter_valid_roi(struct reporter_roi roi)
{
	return roi.x >= 0 && roi.y >= 0 &&
	       roi.w >= 4 && roi.h >= 4 &&
	       roi.x + roi.w <= FRAME_W &&
	       roi.y + roi.h <= FRAME_H &&
	       roi.w * roi.h <= MAX_TEMPLATE_PIXELS;
}

char *reporter_template_path(const char *dir, int state_id)
{
	if (!dir ||
	    (unsigned int)state_id >= (unsigned int)STATE_COUNT)
		return NULL;

	const char *name = state_names[state_id];
	size_t length = strlen(dir) + strlen(name) + 16;

	char *path = bmalloc(length);
	snprintf(path, length, "%s/%s.krt", dir, name);

	return path;
}

void reporter_template_free(struct template_data *model)
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

	/* 几乎均匀的区域无法可靠匹配。 */
	return model->energy / count >= 16.0;
}

/*
 * KRT为本地模板格式。
 * 文件头：magic、版本、帧宽高、ROI x/y/w/h。
 * 数据段：ROI灰度像素。
 */
bool reporter_template_save(const char *path,
			    const struct template_data *model)
{
	if (!path || !model || !model->pixels ||
	    !reporter_valid_roi(model->roi))
		return false;

	struct template_data checked = *model;

	if (!template_statistics(&checked))
		return false;
	
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

bool reporter_template_load(const char *path, struct template_data *model)
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

	model->roi = (struct reporter_roi){
		(int)header[4], (int)header[5],
		(int)header[6], (int)header[7]
	};

	if (!reporter_valid_roi(model->roi)) {
		fclose(file);
		return false;
	}

	size_t count = (size_t)model->roi.w * (size_t)model->roi.h;
	model->pixels = bmalloc(count);

	ok = fread(model->pixels, 1, count, file) == count;
	fclose(file);

	if (!ok || !template_statistics(model)) {
		reporter_template_free(model);
		return false;
	}

	return true;
}

void reporter_to_grayscale(const uint8_t *bgra, uint8_t *gray)
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
 * 归一化互相关。在模板位置附近允许一像素偏移。
 */
double reporter_template_score(const struct template_data *model,
			       const uint8_t *gray)
{
	static const int offsets[5][2] = {
		{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}
	};

	if (!model->pixels)
		return -1.0;

	const struct reporter_roi roi = model->roi;
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

/* ===========================================
 * 保存为 BMP 图像格式进行预览
 * =========================================== */
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

void reporter_save_roi_preview(const struct frame_data *frame,
			       struct reporter_roi roi)
{
	const size_t row_bytes = (size_t)FRAME_W * 4;
	const uint32_t image_bytes = (uint32_t)FRAME_BYTES;

	/* 在副本上画框，识别输入保持不变。 */
	uint8_t *image = bmalloc(FRAME_BYTES);
	memcpy(image, frame->bgra, FRAME_BYTES);

	/* 只绘制确实位于画面内的边框。 */
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

	/* 固定文件名，避免预览文件累积。 */
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
	     reporter_valid_roi(roi) ? "true" : "false", path);

	bfree(path);
	bfree(image);
}
