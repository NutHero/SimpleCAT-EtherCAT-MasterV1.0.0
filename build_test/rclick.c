/* dev_rclick.exe <x> <y> - 在 SimpleCATUI 主窗口客户区坐标模拟鼠标右键单击（测试右键添加波形） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   RECT rc;
   POINT p;

   if (!w) { printf("window not found\n"); return 1; }
   if (argc < 3) { printf("usage: dev_rclick x y\n"); return 1; }
   GetClientRect(w, &rc);
   p.x = rc.left + atoi(argv[1]);
   p.y = rc.top + atoi(argv[2]);
   ClientToScreen(w, &p);

   SetForegroundWindow(w);
   Sleep(200);
   SetCursorPos(p.x, p.y);
   Sleep(150);

   PostMessageW(w, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(atoi(argv[1]), atoi(argv[2])));
   PostMessageW(w, WM_RBUTTONUP, 0, MAKELPARAM(atoi(argv[1]), atoi(argv[2])));
   printf("right-clicked client %s %s (screen %ld,%ld)\n", argv[1], argv[2], p.x, p.y);
   return 0;
}
