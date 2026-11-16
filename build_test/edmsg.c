/* edmsg.exe <vk> - 向 SimpleCATUI 的数值编辑框（ID 95）直接投递 WM_KEYDOWN（绕过
   IsDialogMessageW，用于区分"编辑框处理问题"与"焦点/按键路由问题"） */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
   HWND w, e;
   WPARAM vk;
   if (argc < 2) { printf("usage: edmsg vk\n"); return 1; }
   w = FindWindowW(L"SimpleCATUI", NULL);
   if (!w) { printf("window not found\n"); return 1; }
   e = GetDlgItem(w, 95);
   if (!e) { printf("edit not found\n"); return 1; }
   vk = (WPARAM)atoi(argv[1]);
   printf("edit=%p visible=%d focused=%d\n", (void *)e,
          IsWindowVisible(e) ? 1 : 0,
          GetFocus() == e ? 1 : 0);
   PostMessageW(e, WM_KEYDOWN, vk, 0);
   PostMessageW(e, WM_CHAR, vk == VK_RETURN ? '\r' : vk, 0);
   printf("posted VK %u\n", (unsigned)vk);
   return 0;
}
