/* 向右侧变量树滚动若干格（WM_MOUSEWHEEL 直接投递，坐标取树面板内） */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   RECT rc;
   int clicks = argc > 1 ? atoi(argv[1]) : 10;
   int i;
   POINT p;

   if (!w) { printf("window not found\n"); return 1; }
   GetClientRect(w, &rc);
   p.x = rc.right - 120;      /* 右面板中部（客户区坐标） */
   p.y = rc.top + 400;
   ClientToScreen(w, &p);     /* 转成屏幕坐标再投递 */
   for (i = 0; i < clicks; ++i)
   {
      LPARAM lp = MAKELPARAM(p.x, p.y);
      PostMessageW(w, WM_MOUSEWHEEL, (WPARAM)(((DWORD)-WHEEL_DELTA) << 16), lp);
      Sleep(30);
   }
   printf("scrolled %d clicks at (%ld,%ld)\n", clicks, p.x, p.y);
   return 0;
}
