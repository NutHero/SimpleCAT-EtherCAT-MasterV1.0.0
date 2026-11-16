/* shot.c - 极简屏幕截图工具（GDI BitBlt -> BMP），仅用于验证 UI 渲染 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
   int w = GetSystemMetrics(SM_CXSCREEN);
   int h = GetSystemMetrics(SM_CYSCREEN);
   HDC scr = GetDC(NULL);
   HDC mem = CreateCompatibleDC(scr);
   HBITMAP bmp = CreateCompatibleBitmap(scr, w, h);
   HBITMAP old;
   BITMAPINFOHEADER bih;
   BITMAPFILEHEADER bfh;
   int rowsz = w * 4;
   char *buf;
   FILE *f;

   old = (HBITMAP)SelectObject(mem, bmp);
   BitBlt(mem, 0, 0, w, h, scr, 0, 0, SRCCOPY);
   SelectObject(mem, old);

   memset(&bih, 0, sizeof(bih));
   bih.biSize = sizeof(bih);
   bih.biWidth = w;
   bih.biHeight = -h;
   bih.biPlanes = 1;
   bih.biBitCount = 32;
   bih.biCompression = BI_RGB;

   buf = (char *)malloc(rowsz * (size_t)h);
   GetDIBits(scr, bmp, 0, h, buf, (BITMAPINFO *)&bih, DIB_RGB_COLORS);

   memset(&bfh, 0, sizeof(bfh));
   bfh.bfType = 0x4D42;
   bfh.bfSize = (DWORD)(sizeof(bfh) + sizeof(bih) + (size_t)rowsz * h);
   bfh.bfOffBits = sizeof(bfh) + sizeof(bih);

   f = fopen("shot.bmp", "wb");
   if (f)
   {
      fwrite(&bfh, sizeof(bfh), 1, f);
      fwrite(&bih, sizeof(bih), 1, f);
      fwrite(buf, 1, (size_t)rowsz * h, f);
      fclose(f);
      printf("saved shot.bmp %dx%d\n", w, h);
   }
   else
   {
      printf("open failed\n");
   }

   free(buf);
   DeleteObject(bmp);
   DeleteDC(mem);
   ReleaseDC(NULL, scr);
   return 0;
}
