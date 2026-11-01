// "PS Stream.exe" at the top of the folder: starts the real app from dist, wherever the folder is.

#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
	(void)inst;
	(void)prev;
	(void)cmd;
	(void)show;
	wchar_t dir[MAX_PATH], app[MAX_PATH + 32], line[MAX_PATH + 40];
	GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *slash = wcsrchr(dir, L'\\');
	if(slash)
		*slash = 0;
	wcsncat(dir, L"\\dist", MAX_PATH - wcslen(dir) - 1);
	swprintf(app, MAX_PATH + 32, L"%ls\\PS Stream.exe", dir);
	swprintf(line, MAX_PATH + 40, L"\"%ls\"", app);

	STARTUPINFOW si = { 0 };
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi;
	if(!CreateProcessW(app, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi))
	{
		MessageBoxW(NULL, L"\"dist\" klasörü bulunamadı. Klasörü bütün olarak tut.", L"PS Stream", MB_ICONERROR);
		return 1;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return 0;
}
