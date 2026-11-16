/* 向 SimpleCATUI 主窗口 PostMessage WM_COMMAND（测试菜单命令） */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   UINT id;

   if (!w) { printf("window not found\n"); return 1; }
   if (argc < 2) { printf("usage: dev_postcmd <cmdid>\n"); return 1; }
   id = (UINT)strtoul(argv[1], NULL, 0);
   if (!PostMessageW(w, WM_COMMAND, MAKEWPARAM(id, 0), 0))
   {
      printf("post failed\n");
      return 1;
   }
   printf("posted WM_COMMAND id=%u\n", id);
   return 0;
}
