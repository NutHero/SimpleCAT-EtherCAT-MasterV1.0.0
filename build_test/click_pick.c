/* dev_click.exe <x> <y> - 在 SimpleCATUI 主窗口客户区坐标模拟鼠标点击 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATPick", NULL);
   RECT rc;
   POINT p;
   if (!w) { printf("window not found\n"); return 1; }
   if (argc < 3) { printf("usage: dev_click x y\n"); return 1; }
   GetClientRect(w, &rc);
   p.x = rc.left + atoi(argv[1]);
   p.y = rc.top + atoi(argv[2]);
   ClientToScreen(w, &p);

   SetForegroundWindow(w);
   Sleep(200);
   SetCursorPos(p.x, p.y);
   Sleep(150);

   INPUT in[2];
   memset(in, 0, sizeof(in));
   in[0].type = INPUT_MOUSE;
   in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
   in[1].type = INPUT_MOUSE;
   in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
   SendInput(2, in, sizeof(INPUT));
   printf("clicked %s %s (screen %ld,%ld)\n", argv[1], argv[2], p.x, p.y);
   return 0;
}
