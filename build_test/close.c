/* dev_close.exe - 向 SimpleCATUI 窗口发送 WM_CLOSE，触发正常退出（保存配置） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(void)
{
   HWND h = FindWindowW(L"SimpleCATUI", NULL);
   if (h == NULL)
   {
      printf("window not found\n");
      return 1;
   }
   PostMessageW(h, WM_CLOSE, 0, 0);
   printf("WM_CLOSE posted\n");
   Sleep(600);
   return 0;
}
