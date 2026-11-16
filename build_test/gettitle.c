/* 读取 SimpleCAT UI 窗口标题（调试信息载体） */
#include <windows.h>
#include <stdio.h>

int main(void)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   char t[256] = "(none)";

   if (!w) { printf("window not found\n"); return 1; }
   GetWindowTextA(w, t, sizeof(t));
   printf("title: %s\n", t);
   return 0;
}
