/* enumall.exe <exe-name> - 列出指定进程名的全部顶层窗口（类名/标题/矩形） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>

static DWORD g_targets[64];
static int g_nt = 0;

static void collect_pids(const WCHAR *exe)
{
   HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   PROCESSENTRY32W pe;
   BOOL ok;
   if (s == INVALID_HANDLE_VALUE) return;
   pe.dwSize = sizeof(pe);
   for (ok = Process32FirstW(s, &pe); ok && g_nt < 64; ok = Process32NextW(s, &pe))
      if (_wcsicmp(pe.szExeFile, exe) == 0)
         g_targets[g_nt++] = pe.th32ProcessID;
   CloseHandle(s);
}

static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
   DWORD pid = 0;
   WCHAR cls[256], txt[256];
   RECT r;
   int i;
   (void)lp;
   GetWindowThreadProcessId(w, &pid);
   for (i = 0; i < g_nt; ++i)
      if (g_targets[i] == pid) break;
   if (i >= g_nt) return TRUE;
   GetClassNameW(w, cls, 256);
   GetWindowTextW(w, txt, 256);
   GetWindowRect(w, &r);
   wprintf(L"pid=%lu hwnd=%p cls=%s txt=%s rect=(%ld,%ld)-(%ld,%ld) vis=%d\r\n",
           pid, w, cls, txt, r.left, r.top, r.right, r.bottom, IsWindowVisible(w));
   return TRUE;
}

int main(int argc, char **argv)
{
   if (argc < 2) { printf("usage: enumall exe-name\n"); return 1; }
   wchar_t wexe[260];
   MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wexe, 260);
   collect_pids(wexe);
   printf("pids found: %d\n", g_nt);
   EnumWindows(enum_cb, 0);
   return 0;
}
