/* 抓取 SimpleCAT UI 窗口客户区保存为 BMP（client 坐标，位置无关） */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
   HWND w = FindWindowW(L"SimpleCATUI", NULL);
   HDC wdc, mdc;
   HBITMAP bmp, obmp;
   RECT rc;
   BITMAPINFO bi;
   BITMAPFILEHEADER fh;
   FILE *f;
   void *bits;
   const char *path = argc > 1 ? argv[1] : "win_client.bmp";

   if (!w) { printf("window not found\n"); return 1; }
   GetClientRect(w, &rc);
   wdc = GetDC(w);
   mdc = CreateCompatibleDC(wdc);
   bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
   obmp = (HBITMAP)SelectObject(mdc, bmp);
   PrintWindow(w, mdc, PW_CLIENTONLY);

   memset(&bi, 0, sizeof(bi));
   bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   bi.bmiHeader.biWidth = rc.right;
   bi.bmiHeader.biHeight = -rc.bottom;      /* top-down */
   bi.bmiHeader.biPlanes = 1;
   bi.bmiHeader.biBitCount = 24;
   bi.bmiHeader.biCompression = BI_RGB;
   {
      int stride = ((rc.right * 3 + 3) / 4) * 4;
      bits = malloc((size_t)stride * rc.bottom);
      GetDIBits(mdc, bmp, 0, rc.bottom, bits, &bi, DIB_RGB_COLORS);

      f = fopen(path, "wb");
      if (!f) { printf("open failed\n"); return 1; }
      memset(&fh, 0, sizeof(fh));
      fh.bfType = 0x4D42;
      fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
      fh.bfSize = (DWORD)(fh.bfOffBits + (size_t)stride * rc.bottom);
      fwrite(&fh, sizeof(fh), 1, f);
      fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
      fwrite(bits, 1, (size_t)stride * rc.bottom, f);
      fclose(f);
      printf("saved %s %dx%d\n", path, (int)rc.right, (int)rc.bottom);
   }
   SelectObject(mdc, obmp);
   DeleteObject(bmp);
   DeleteDC(mdc);
   ReleaseDC(w, wdc);
   return 0;
}
