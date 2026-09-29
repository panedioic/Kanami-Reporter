// src/reporter-import.h - Resource import dialog for Kanami Reporter filter.

#include "reporter-import.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <windows.h>
#include <commdlg.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IMPORT_PATH_MAX (MAX_PATH * 2)

/* ---------- 路径辅助 ---------- */

static wchar_t *widen(const char *utf8)
{
	if (!utf8)
		return NULL;

	int count = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);

	if (count <= 0)
		return NULL;

	wchar_t *out = bmalloc((size_t)count * sizeof(*out));
	if (!out)
		return NULL;

	if (MultiByteToWideChar(
		    CP_UTF8, MB_ERR_INVALID_CHARS,
		    utf8, -1, out, count) != count) {
		bfree(out);
		return NULL;
	}

	return out;
}

static bool copy_wide(wchar_t *out, size_t count, const wchar_t *src)
{
	size_t length = wcslen(src);

	if (length + 1 > count)
		return false;

	memcpy(out, src, (length + 1) * sizeof(wchar_t));
	return true;
}

static bool join_path(wchar_t *out, size_t count,
		      const wchar_t *parent, const wchar_t *child)
{
	int written = _snwprintf_s(
		out, count, _TRUNCATE, L"%ls\\%ls", parent, child);

	return written > 0;
}

static bool is_directory(const wchar_t *path)
{
	DWORD attributes = GetFileAttributesW(path);

	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* 逐级创建目录，不依赖Shell API。 */
static bool ensure_dir_tree(const wchar_t *path)
{
	if (!path || !path[0])
		return false;

	size_t length = wcslen(path);
	wchar_t *buffer = bmalloc((length + 1) * sizeof(*buffer));

	if (!buffer)
		return false;

	memcpy(buffer, path, (length + 1) * sizeof(wchar_t));

	for (size_t i = 0; i < length; i++) {
		if (buffer[i] != L'\\' && buffer[i] != L'/')
			continue;

		/* 跳过盘符后的分隔符，例如 C:\ */
		if (i == 2 && buffer[1] == L':')
			continue;

		wchar_t saved = buffer[i];
		buffer[i] = L'\0';

		if (!CreateDirectoryW(buffer, NULL) &&
		    GetLastError() != ERROR_ALREADY_EXISTS) {
			bfree(buffer);
			return false;
		}

		buffer[i] = saved;
	}

	bool ok = CreateDirectoryW(buffer, NULL) ||
		  GetLastError() == ERROR_ALREADY_EXISTS;

	bfree(buffer);
	return ok;
}

/* ---------- 目录复制与删除 ---------- */

static bool copy_tree(const wchar_t *src, const wchar_t *dst,
		      unsigned long *files)
{
	if (!ensure_dir_tree(dst))
		return false;

	wchar_t pattern[IMPORT_PATH_MAX];
	if (!join_path(pattern, ARRAYSIZE(pattern), src, L"*"))
		return false;

	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(pattern, &data);

	if (find == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND;

	bool ok = true;

	do {
		if (wcscmp(data.cFileName, L".") == 0 ||
		    wcscmp(data.cFileName, L"..") == 0)
			continue;

		wchar_t src_child[IMPORT_PATH_MAX];
		wchar_t dst_child[IMPORT_PATH_MAX];

		if (!join_path(src_child, ARRAYSIZE(src_child),
			       src, data.cFileName) ||
		    !join_path(dst_child, ARRAYSIZE(dst_child),
			       dst, data.cFileName)) {
			ok = false;
			break;
		}

		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			/* continue 会执行while条件，正确推进枚举。 */
			if (!copy_tree(src_child, dst_child, files)) {
				ok = false;
				break;
			}

			continue;
		}

		if (!CopyFileW(src_child, dst_child, FALSE)) {
			ok = false;
			break;
		}

		(*files)++;
	} while (FindNextFileW(find, &data));

	FindClose(find);
	return ok;
}

static void remove_tree(const wchar_t *path)
{
	wchar_t pattern[IMPORT_PATH_MAX];
	if (!join_path(pattern, ARRAYSIZE(pattern), path, L"*"))
		return;

	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(pattern, &data);

	if (find != INVALID_HANDLE_VALUE) {
		do {
			if (wcscmp(data.cFileName, L".") == 0 ||
			    wcscmp(data.cFileName, L"..") == 0)
				continue;

			wchar_t child[IMPORT_PATH_MAX];

			if (!join_path(child, ARRAYSIZE(child),
				       path, data.cFileName))
				break;

			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				remove_tree(child);
				continue;
			}

			SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(child);
		} while (FindNextFileW(find, &data));

		FindClose(find);
	}

	RemoveDirectoryW(path);
}

/* ---------- 解压 ---------- */

static bool run_process_and_wait(wchar_t *command_line, DWORD *exit_code)
{
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;

	ZeroMemory(&startup, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;

	ZeroMemory(&process, sizeof(process));

	/*
	 * CreateProcessW可能修改命令行缓冲区，
	 * 因此调用方必须传入可写内存。
	 */
	if (!CreateProcessW(NULL, command_line, NULL, NULL, FALSE,
			    CREATE_NO_WINDOW, NULL, NULL,
			    &startup, &process))
		return false;

	DWORD wait = WaitForSingleObject(process.hProcess, 120000);
	bool ok = wait == WAIT_OBJECT_0;

	if (!ok && wait == WAIT_TIMEOUT)
		TerminateProcess(process.hProcess, 1);

	if (ok && exit_code && !GetExitCodeProcess(process.hProcess, exit_code))
		ok = false;

	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return ok;
}

static bool extract_zip(const wchar_t *zip, const wchar_t *destination)
{
	wchar_t system_dir[MAX_PATH];

	if (!GetSystemDirectoryW(system_dir, (UINT)ARRAYSIZE(system_dir)))
		return false;

	wchar_t tar_path[IMPORT_PATH_MAX];
	if (!join_path(tar_path, ARRAYSIZE(tar_path),
		       system_dir, L"tar.exe"))
		return false;

	if (GetFileAttributesW(tar_path) == INVALID_FILE_ATTRIBUTES) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [IMPORT] tar.exe not found: %ls",
		     tar_path);
		return false;
	}

	/*
	 * tar.exe 是系统自带的bsdtar，可直接解析zip。
	 * -x 解包，-f 指定压缩包，-C 指定目标目录。
	 */
	wchar_t command[8192];

	int written = _snwprintf_s(
		command, ARRAYSIZE(command), _TRUNCATE,
		L"\"%ls\" -x -f \"%ls\" -C \"%ls\"",
		tar_path, zip, destination);

	if (written <= 0)
		return false;

	DWORD exit_code = 1;

	if (!run_process_and_wait(command, &exit_code))
		return false;

	if (exit_code != 0) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [IMPORT] tar exit code: %lu",
		     (unsigned long)exit_code);
		return false;
	}

	return true;
}

/* ---------- 资源定位与导入 ---------- */

static bool has_resources(const wchar_t *directory)
{
	wchar_t probe[IMPORT_PATH_MAX];

	if (join_path(probe, ARRAYSIZE(probe), directory, L"templates") &&
	    is_directory(probe))
		return true;

	if (join_path(probe, ARRAYSIZE(probe), directory, L"voices") &&
	    is_directory(probe))
		return true;

	return false;
}

/*
 * 兼容两种压缩包结构：
 *   1) 根目录直接是 templates / voices
 *   2) 根目录是 Kanami-Reporter/，里面是 templates / voices
 */
static bool find_source_root(const wchar_t *root, wchar_t *out, size_t count)
{
	if (has_resources(root))
		return copy_wide(out, count, root);

	wchar_t pattern[IMPORT_PATH_MAX];
	if (!join_path(pattern, ARRAYSIZE(pattern), root, L"*"))
		return false;

	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(pattern, &data);

	if (find == INVALID_HANDLE_VALUE)
		return false;

	size_t subdirectory_count = 0;
	wchar_t only_subdirectory[IMPORT_PATH_MAX] = {0};

	do {
		if (wcscmp(data.cFileName, L".") == 0 ||
		    wcscmp(data.cFileName, L"..") == 0)
			continue;

		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;

		subdirectory_count++;

		if (subdirectory_count == 1) {
			join_path(only_subdirectory,
				  ARRAYSIZE(only_subdirectory),
				  root, data.cFileName);
		} else {
			break;
		}
	} while (FindNextFileW(find, &data));

	FindClose(find);

	if (subdirectory_count == 1 && has_resources(only_subdirectory))
		return copy_wide(out, count, only_subdirectory);

	return false;
}

static unsigned long import_subdirectory(const wchar_t *source_root,
					 const wchar_t *config_dir,
					 const wchar_t *name)
{
	wchar_t src[IMPORT_PATH_MAX];
	wchar_t dst[IMPORT_PATH_MAX];

	if (!join_path(src, ARRAYSIZE(src), source_root, name) ||
	    !is_directory(src))
		return 0;

	if (!join_path(dst, ARRAYSIZE(dst), config_dir, name))
		return 0;

	unsigned long files = 0;

	if (!copy_tree(src, dst, &files)) {
		blog(LOG_WARNING,
		     "[Kanami-Reporter] [IMPORT] Failed copying: %ls",
		     name);
	}

	return files;
}

/* ---------- 对外入口 ---------- */

bool reporter_import_resources_from_dialog(void)
{
	/* 属性按钮在OBS界面线程执行，使用当前活动窗口作为属主。 */
	HWND owner = GetForegroundWindow();

	wchar_t zip[IMPORT_PATH_MAX] = {0};

	OPENFILENAMEW dialog;
	ZeroMemory(&dialog, sizeof(dialog));

	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter =
		L"资源包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0\0";
	dialog.lpstrFile = zip;
	dialog.nMaxFile = (DWORD)ARRAYSIZE(zip);
	dialog.lpstrTitle = L"选择 Kanami-Reporter 资源包";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
		       OFN_NOCHANGEDIR;

	if (!GetOpenFileNameW(&dialog)) {
		/* 用户取消，不视为错误。 */
		return false;
	}

	wchar_t temp_path[MAX_PATH];

	if (!GetTempPathW((DWORD)ARRAYSIZE(temp_path), temp_path))
		return false;

	wchar_t temp_dir[IMPORT_PATH_MAX];

	int written = _snwprintf_s(
		temp_dir, ARRAYSIZE(temp_dir), _TRUNCATE,
		L"%lsKanamiReporterImport-%lu",
		temp_path, (unsigned long)GetCurrentProcessId());

	if (written <= 0)
		return false;

	/* 清理上次运行残留。 */
	remove_tree(temp_dir);

	if (!ensure_dir_tree(temp_dir))
		return false;

	if (!extract_zip(zip, temp_dir)) {
		MessageBoxW(
			owner,
			L"解压失败。\n\n"
			L"请确认系统自带的 tar.exe 可用，"
			L"或手动解压该资源包。",
			L"Kanami-Reporter",
			MB_ICONERROR | MB_OK);

		remove_tree(temp_dir);
		return false;
	}

	wchar_t source_root[IMPORT_PATH_MAX];

	if (!find_source_root(temp_dir, source_root,
			      ARRAYSIZE(source_root))) {
		MessageBoxW(
			owner,
			L"压缩包中没有找到 templates 或 voices 目录。\n\n"
			L"请确认选择的是插件生成的资源包。",
			L"Kanami-Reporter",
			MB_ICONWARNING | MB_OK);

		remove_tree(temp_dir);
		return false;
	}

	char *config_utf8 = obs_module_config_path("");
	if (!config_utf8) {
		remove_tree(temp_dir);
		return false;
	}

	/* 去掉结尾的分隔符，便于拼接子目录。 */
	size_t config_length = strlen(config_utf8);

	while (config_length > 0 &&
	       (config_utf8[config_length - 1] == '/' ||
		config_utf8[config_length - 1] == '\\')) {
		config_utf8[--config_length] = '\0';
	}

	wchar_t *config_dir = widen(config_utf8);
	bfree(config_utf8);

	if (!config_dir) {
		remove_tree(temp_dir);
		return false;
	}

	bool ok = false;

	if (ensure_dir_tree(config_dir)) {
		unsigned long templates = import_subdirectory(
			source_root, config_dir, L"templates");
		unsigned long voices = import_subdirectory(
			source_root, config_dir, L"voices");

		blog(LOG_INFO,
		     "[Kanami-Reporter] [IMPORT] templates=%lu voices=%lu",
		     templates, voices);

		wchar_t message[1024];

		_snwprintf_s(
			message, ARRAYSIZE(message), _TRUNCATE,
			L"资源导入完成。\n\n"
			L"模板文件：%lu 个\n"
			L"语音文件：%lu 个\n\n"
			L"模板已在后台重新加载，无需重启 OBS。",
			templates, voices);

		MessageBoxW(owner, message, L"Kanami-Reporter",
			    MB_ICONINFORMATION | MB_OK);

		ok = templates > 0 || voices > 0;
	} else {
		MessageBoxW(
			owner,
			L"无法创建插件配置目录。",
			L"Kanami-Reporter",
			MB_ICONERROR | MB_OK);
	}

	bfree(config_dir);
	remove_tree(temp_dir);

	return ok;
}