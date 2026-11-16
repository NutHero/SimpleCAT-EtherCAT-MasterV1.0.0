/* dblhwnd.exe <hwnd-hex> <x> <y> - 向指定窗口投递双击消息序列（客户区坐标） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w;
   LPARAM lp;
   if (argc < 4) { printf("usage: dblhwnd hwnd x y\n"); return 1; }
   w = (HWND)(uintptr_t)strtoull(argv[1], NULL, 16);
   if (!IsWindow(w)) { printf("not a window\n"); return 1; }
   lp = MAKELPARAM(atoi(argv[2]), atoi(argv[3]));
   SetForegroundWindow(w);
   Sleep(150);
   PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   PostMessageW(w, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
   PostMessageW(w, WM_LBUTTONUP, 0, lp);
   printf("double-clicked hwnd=%p client(%d,%d)\n", (void*)w, atoi(argv[2]), atoi(argv[3]));
   return 0;
}
