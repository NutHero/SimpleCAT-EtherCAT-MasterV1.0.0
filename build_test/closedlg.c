#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int main(void){
   HWND d = FindWindowW(L"#32770", NULL);
   if (d){ PostMessageW(d, WM_CLOSE, 0, 0); return 0; }
   return 1;
}
