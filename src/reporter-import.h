// src/reporter-import.h - Resource import dialog for Kanami Reporter filter.

#pragma once

#include <stdbool.h>

/*
 * 打开系统文件选择器，导入资源压缩包。
 * 返回true表示至少导入了 templates 或 voices 中的一个。
 */
bool reporter_import_resources_from_dialog(void);