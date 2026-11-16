/* clickmsg.exe <x> <y> - 直接向 SimpleCATUI 主窗口投递 WM_LBUTTONDOWN/UP（绕过前台焦点） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   LPARAM lp;
   if (!w) { printf("window not found\n"); return 1; }
   if (argc < 3) { printf("usage: clickmsg x y\n"); return 1; }
   lp = MAKELPARAM(atoi(argv[1]), atoi(argv[2]));
   SetForegroundWindow(w);
   Sleep(150);
   PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   printf("posted %s %s\n", argv[1], argv[2]);
   return 0;
}
