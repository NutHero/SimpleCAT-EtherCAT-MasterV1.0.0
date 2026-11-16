/* fgcat.exe - 把 SimpleCATUI 主窗口置前 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   if (!w) { printf("not found\n"); return 1; }
   SetForegroundWindow(w);
   Sleep(400);
   printf("foregrounded hwnd=%p fg=%d\n", (void*)w, GetForegroundWindow() == w);
   return 0;
}
