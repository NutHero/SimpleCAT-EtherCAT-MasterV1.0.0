/* dblval.exe <x> <y> - 在指定物理屏幕坐标处对 SimpleCATUI 窗口投递双击序列
   （DPI aware，物理像素坐标；自动换算为客户区坐标；只投递给坐标命中的窗口） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   POINT pt;
   HWND w;
   RECT wr, cr;
   LPARAM lp;
   int cx, cy;

   if (argc < 3) { printf("usage: dblval x y\n"); return 1; }
   SetProcessDPIAware();
   pt.x = atoi(argv[1]); pt.y = atoi(argv[2]);
   w = WindowFromPoint(pt);
   if (!w) { printf("no window at point\n"); return 1; }
   {
      WCHAR cls[256];
      GetClassNameW(w, cls, 256);
      if (wcscmp(cls, L"SimpleCATUI") != 0)
      {
         wprintf(L"unexpected class: %s\n", cls);
         return 1;
      }
   }
   GetWindowRect(w, &wr);
   GetClientRect(w, &cr);
   ScreenToClient(w, &pt);
   cx = pt.x; cy = pt.y;
   lp = MAKELPARAM(cx, cy);
   SetForegroundWindow(w);
   Sleep(150);
   PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   PostMessageW(w, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   printf("win=(%ld,%ld)-(%ld,%ld) client=%dx%d double-clicked client(%d,%d)\n",
          wr.left, wr.top, wr.right, wr.bottom, cr.right, cr.bottom, cx, cy);
   return 0;
}
