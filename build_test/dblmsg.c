/* dblmsg.exe <x> <y> - 向 SimpleCATUI 主窗口投递双击消息序列（客户区坐标） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   LPARAM lp;
   if (!w) { printf("window not found\n"); return 1; }
   if (argc < 3) { printf("usage: dblmsg x y\n"); return 1; }
   lp = MAKELPARAM(atoi(argv[1]), atoi(argv[2]));
   SetForegroundWindow(w);
   Sleep(150);
   PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   PostMessageW(w, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   printf("double-clicked %s %s\n", argv[1], argv[2]);
   return 0;
}
