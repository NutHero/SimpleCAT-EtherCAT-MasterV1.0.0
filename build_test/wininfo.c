/* wininfo.exe - 枚举 SimpleCATUI 进程的全部顶层窗口 + 主窗口状态 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static DWORD g_pid = 0;
static WCHAR cls[256], txt[256];

static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
   DWORD pid = 0;
   RECT r;
   GetWindowThreadProcessId(w, &pid);
   if (pid != g_pid || GetParent(w) != NULL)
      return TRUE;
   GetClassNameW(w, cls, 256);
   if (!GetWindowTextW(w, txt, 256)) txt[0] = 0;
   GetWindowRect(w, &r);
   wprintf(L"hwnd=%p cls=%s txt=%s rect=(%ld,%ld)-(%ld,%ld) vis=%d en=%d\r\n",
           w, cls, txt, r.left, r.top, r.right, r.bottom,
           IsWindowVisible(w), IsWindowEnabled(w));
   return TRUE;
}

int main(void)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   RECT r;
   if (!w) { printf("main not found\n"); return 1; }
   GetWindowThreadProcessId(w, &g_pid);
   GetWindowRect(w, &r);
   wprintf(L"main hwnd=%p rect=(%ld,%ld)-(%ld,%ld) vis=%d en=%d fg=%d\r\n",
           w, r.left, r.top, r.right, r.bottom,
           IsWindowVisible(w), IsWindowEnabled(w),
           GetForegroundWindow() == w);
   EnumWindows(enum_cb, 0);
   return 0;
}
