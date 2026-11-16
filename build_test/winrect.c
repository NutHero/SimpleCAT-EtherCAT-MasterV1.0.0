/* winrect.exe - 列出所有 SimpleCATUI 顶层窗口（DPI aware 物理坐标），
   并标注所属进程名，附带客户区原点的物理屏幕坐标 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>

static DWORD pid_to_name(DWORD pid, WCHAR *name, DWORD n)
{
   HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   PROCESSENTRY32W pe;
   BOOL ok;
   name[0] = 0;
   if (s == INVALID_HANDLE_VALUE) return 0;
   pe.dwSize = sizeof(pe);
   for (ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
      if (pe.th32ProcessID == pid)
      {
         wcsncpy_s(name, n, pe.szExeFile, _TRUNCATE);
         break;
      }
   CloseHandle(s);
   return pid;
}

static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
   DWORD pid = 0;
   WCHAR cls[256], txt[256], exe[260];
   RECT wr, cr;
   POINT po;
   (void)lp;
   GetWindowThreadProcessId(w, &pid);
   GetClassNameW(w, cls, 256);
   if (wcscmp(cls, L"SimpleCATUI") != 0) return TRUE;
   pid_to_name(pid, exe, 260);
   GetWindowTextW(w, txt, 256);
   GetWindowRect(w, &wr);
   GetClientRect(w, &cr);
   po.x = 0; po.y = 0;
   ClientToScreen(w, &po);
   wprintf(L"hwnd=%p exe=%s rect=(%ld,%ld)-(%ld,%ld) client=%dx%d client_origin=(%ld,%ld) vis=%d\r\n",
           w, exe, wr.left, wr.top, wr.right, wr.bottom,
           cr.right, cr.bottom, po.x, po.y, IsWindowVisible(w));
   return TRUE;
}

int main(void)
{
   SetProcessDPIAware();
   EnumWindows(enum_cb, 0);
   return 0;
}
