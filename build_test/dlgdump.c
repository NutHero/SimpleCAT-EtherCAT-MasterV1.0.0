/* dlgdump.exe - 列出所有可见 #32770 对话框的标题与内部静态文本 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static WCHAR txt[1024];

static BOOL CALLBACK cb(HWND d, LPARAM lp)
{
   WCHAR cls[64];
   (void)lp;
   GetClassNameW(d, cls, 64);
   if (wcscmp(cls, L"#32770") == 0 && IsWindowVisible(d))
   {
      HWND st = GetDlgItem(d, 0xFFFF);   /* 大文本静态框 */
      GetWindowTextW(d, txt, 256);
      wprintf(L"dlg title=%s\n", txt);
      if (st)
      {
         GetWindowTextW(st, txt, 1024);
         wprintf(L"dlg text=%.180s\n", txt);
      }
      wprintf(L"----\n");
   }
   return TRUE;
}

int main(void)
{
   EnumWindows(cb, 0);
   printf("done\n");
   return 0;
}
