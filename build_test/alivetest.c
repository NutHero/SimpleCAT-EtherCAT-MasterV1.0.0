/* alivetest.exe - 测试主窗口 UI 线程是否在泵消息（SendMessageTimeout WM_NULL） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(void)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   DWORD res;
   if (!w) { printf("not found\n"); return 1; }
   res = 0;
   if (SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_BLOCK, 2000, &res))
      printf("ALIVE (%lu ms timeout flag ok)\n", res);
   else
      printf("HUNG or timeout (err=%lu)\n", GetLastError());
   return 0;
}
