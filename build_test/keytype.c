/* keytype.exe <text> [-enter] - 向前台窗口真实发送键盘输入（走系统消息循环，验证
   IsDialogMessageW 路径下的回车）。-enter = 结尾追加回车 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void tap(WORD vk, int shift)
{
   INPUT in[4];
   int n = 0;
   memset(in, 0, sizeof(in));
   if (shift)
   {
      in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; n++;
   }
   in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
   in[n].type = INPUT_KEYBOARD; in[n].ki.dwFlags = KEYEVENTF_KEYUP; in[n].ki.wVk = vk; n++;
   if (shift)
   {
      in[n].type = INPUT_KEYBOARD; in[n].ki.dwFlags = KEYEVENTF_KEYUP;
      in[n].ki.wVk = VK_SHIFT; n++;
   }
   SendInput((UINT)n, in, sizeof(INPUT));
}

static void type_char(char c)
{
   SHORT s = VkKeyScanA(c);
   if (s == -1) return;
   tap((WORD)(s & 0xFF), (s & 0x0100) ? 1 : 0);
}

int main(int argc, char **argv)
{
   int i, enter = 0;
   HWND w, fg;
   DWORD thisTid, fgTid;
   if (argc < 2) { printf("usage: keytype text [-enter]\n"); return 1; }
   if (argc >= 3 && strcmp(argv[2], "-enter") == 0) enter = 1;

   w = FindWindowW(L"SimpleCATUI", NULL);
   if (!w) { printf("window not found\n"); return 1; }

   /* 跨进程抢前台：附加到前台窗口的输入队列后再激活，避免 SetForegroundWindow 被拒 */
   fg = GetForegroundWindow();
   thisTid = GetCurrentThreadId();
   fgTid = GetWindowThreadProcessId(fg, NULL);
   if (fgTid != thisTid)
      AttachThreadInput(thisTid, fgTid, TRUE);
   SetForegroundWindow(w);
   Sleep(150);
   {
      HWND e = GetDlgItem(w, 95);
      if (e && IsWindowVisible(e))
         SetFocus(e);      /* 附加输入队列后可以跨进程设焦点 */
   }
   if (fgTid != thisTid)
      AttachThreadInput(thisTid, fgTid, FALSE);

   fg = GetForegroundWindow();
   printf("foreground=%p target=%p %s\n", (void *)fg, (void *)w,
          fg == w ? "OK" : "NOT-FOREGROUND");

   for (i = 0; argv[1][i]; ++i)
      type_char(argv[1][i]);

   if (enter)
   {
      Sleep(100);
      tap(VK_RETURN, 0);
   }
   printf("typed \"%s\"%s\n", argv[1], enter ? " + ENTER" : "");
   return 0;
}
