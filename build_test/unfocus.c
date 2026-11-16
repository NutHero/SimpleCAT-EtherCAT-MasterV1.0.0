/* 激活除 SimpleCATUI 之外的下一个可见顶层窗口（用于测试失焦悬停） */
#include <windows.h>
#include <stdio.h>

static HWND g_skip;
static HWND g_best;

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp)
{
   (void)lp;
   if (h == g_skip) return TRUE;
   if (!IsWindowVisible(h)) return TRUE;
   if (IsIconic(h)) return TRUE;
   {
      RECT r;
      if (!GetWindowRect(h, &r)) return TRUE;
      if (r.right - r.left < 100 || r.bottom - r.top < 80) return TRUE;
   }
   {
      wchar_t cls[64];
      if (!GetClassNameW(h, cls, 64)) return TRUE;
      /* 跳过常见系统辅助窗口 */
      if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Progman")) return TRUE;
   }
   g_best = h;
   return FALSE;   /* 取 Z 序第一个（最新的） */
}

int main(void)
{
   g_skip = FindWindowW(L"SimpleCATUI", NULL);
   if (!g_skip) { printf("simplecat not found\n"); return 1; }
   EnumWindows(enum_proc, 0);
   if (!g_best) { printf("no other window\n"); return 1; }
   {
      wchar_t t[128];
      GetWindowTextW(g_best, t, 128);
      wprintf(L"activate: %s\n", t);
   }
   SetForegroundWindow(g_best);
   printf("fg now: %p (simplecat=%p)\n",
          (void *)GetForegroundWindow(), (void *)g_skip);
   return 0;
}
