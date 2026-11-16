#include <windows.h>
#include <stdio.h>
int main(void){
  HWND w = FindWindowW(L"SimpleCATUI", NULL);
  if(!w){ printf("window not found\n"); return 1; }
  ShowWindow(w, SW_RESTORE);
  SetForegroundWindow(w);
  RECT r; GetWindowRect(w,&r);
  printf("found %dx%d at %ld,%ld\n", r.right-r.left, r.bottom-r.top, r.left, r.top);
  return 0;
}
