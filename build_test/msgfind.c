/* msgfind.exe - 列出本进程外所有可见 #32770 对话框标题 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
static WCHAR txt[256];
static BOOL CALLBACK cb(HWND w, LPARAM lp)
{
   WCHAR cls[64];
   GetClassNameW(w, cls, 64);
   if (wcscmp(cls, L"#32770") == 0 && IsWindowVisible(w))
   {
      GetWindowTextW(w, txt, 256);
      wprintf(L"dlg hwnd=%p title=%s\r\n", w, txt);
   }
   return TRUE;
}
int main(void)
{
   EnumWindows(cb, 0);
   printf("done\n");
   return 0;
}
