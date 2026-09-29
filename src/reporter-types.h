// src/reporter-types.h - Kanami Reporter filter types.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 识别坐标直接使用来源分辨率，每100ms截取一帧。 */
#define FRAME_W 1920
#define FRAME_H 1080
#define FRAME_BYTES ((size_t)FRAME_W * FRAME_H * 4)
#define CAPTURE_NS 100000000ULL

/* 单个事件模板的面积上限，不是整帧截图上限。 */
#define MAX_TEMPLATE_PIXELS 262144

struct reporter_roi {
	int x;
	int y;
	int w;
	int h;
};

struct template_data {
	struct reporter_roi roi;
	uint8_t *pixels;
	double mean;
	double energy;
};

struct frame_data {
	uint8_t *bgra;

	/*
	 * 借用工作线程的灰度缓冲区。
	 * frame释放时不释放gray；由工作线程退出时统一释放。
	 */
	const uint8_t *gray;
	uint32_t width;
	uint32_t height;

	uint64_t timestamp;
	bool recognize;
	double threshold;
};

void reporter_frame_free(struct frame_data *frame);