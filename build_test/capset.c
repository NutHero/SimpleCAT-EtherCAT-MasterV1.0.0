/* capset.c -- 向缓冲区上限编辑框（ID 96）设置文本并发送回车 */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
   HWND main, ed;
   const char *val = (argc > 1) ? argv[1] : "50000";
   wchar_t wv[32];

   main = FindWindowW(L"SimpleCATUI", NULL);
   if (main == NULL) { printf("main window not found\n"); return 1; }
   ed = GetDlgItem(main, 96);
   if (ed == NULL) { printf("edit 96 not found\n"); return 1; }

   MultiByteToWideChar(CP_UTF8, 0, val, -1, wv, 32);
   SendMessageW(ed, WM_SETTEXT, 0, (LPARAM)wv);
   Sleep(300);
   PostMessageW(ed, WM_KEYDOWN, VK_RETURN, 0);
   Sleep(800);
   printf("sent value %s + ENTER\n", val);
   return 0;
}
