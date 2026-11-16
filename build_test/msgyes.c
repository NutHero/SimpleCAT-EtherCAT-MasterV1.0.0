/* msgyes.exe <title> - 向标题匹配的 #32770 对话框发送 IDYES/IDOK */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
   HWND w;
   if (argc < 2) { printf("usage: msgyes title\n"); return 1; }
   {
      wchar_t wt[128];
      MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wt, 128);
      w = FindWindowW(L"#32770", NULL);
   }
   if (!w) { printf("dialog not found\n"); return 1; }
   PostMessageW(w, WM_COMMAND, IDYES, 0);
   printf("IDYES posted to dialog\n");
   return 0;
}
