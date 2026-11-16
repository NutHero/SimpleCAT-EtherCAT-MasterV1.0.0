#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(void){
   HWND d = FindWindowW(L"#32770", NULL);
   wchar_t t[128]={0};
   if (d){ GetWindowTextW(d,t,128); wprintf(L"DIALOG: %s\n", t); }
   else printf("no dialog\n");
   return 0;
}
