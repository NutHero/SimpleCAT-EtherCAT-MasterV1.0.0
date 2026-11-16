/*
 * ui_config.c - 界面配置持久化（SimpleCAT.ini，位于 exe 同目录）
 *
 * 只用 Win32 Profile API（GetPrivateProfileIntW / WritePrivateProfileStringW），
 * 无第三方依赖；所有读写失败都静默降级为默认值，绝不影响主站运行。
 */
#include "ui_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CFG_NAME  L"SimpleCAT.ini"
#define CFG_SEC   L"ui"

/* 取 exe 同目录下 ini 的完整路径（宽字符）；成功返回 1 */
static int cfg_wide_path(wchar_t *out, int cch)
{
   wchar_t exe[MAX_PATH];
   wchar_t *slash;
   DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);

   if (n == 0 || n >= MAX_PATH)
      return 0;
   slash = wcsrchr(exe, L'\\');
   if (slash == NULL)
      return 0;
   slash[1] = L'\0';
   if ((int)(wcslen(exe) + wcslen(CFG_NAME) + 1) > cch)
      return 0;
   wcscpy(out, exe);
   wcscat(out, CFG_NAME);
   return 1;
}

void ui_config_path(char *out, int cb)
{
   wchar_t wp[MAX_PATH];

   if (out == NULL || cb <= 0)
      return;
   out[0] = '\0';
   if (cfg_wide_path(wp, MAX_PATH))
      WideCharToMultiByte(CP_UTF8, 0, wp, -1, out, cb, NULL, NULL);
}

void ui_config_defaults(ui_config_t *c)
{
   if (c == NULL)
      return;
   memset(c, 0, sizeof(*c));
   c->skin = 1;          /* 浅色 */
   c->slave_mode = -1;   /* 未设置：连接时按从站是否支持 DC 自动选择 */
   c->sample_ms = 100;
   c->wave_buf = 100000;
   c->win_x = 0;
   c->win_y = 0;
   c->win_w = 0;         /* 0 = 由界面使用默认宽高 */
   c->win_h = 0;
   c->win_max = 0;
   c->last_flash_slave = 1;
   c->lang = 0;          /* 中文 */
}

int ui_config_load(ui_config_t *c)
{
   wchar_t wp[MAX_PATH];
   wchar_t buf[512];
   char utf8[512];

   if (c == NULL)
      return 0;
   ui_config_defaults(c);
   if (!cfg_wide_path(wp, MAX_PATH))
      return 0;
   if (GetFileAttributesW(wp) == INVALID_FILE_ATTRIBUTES)
      return 0;   /* 首次运行：用默认值 */

   c->skin = (int)GetPrivateProfileIntW(CFG_SEC, L"skin", c->skin, wp);
   c->slave_mode = (int)GetPrivateProfileIntW(CFG_SEC, L"slave_mode", c->slave_mode, wp);
   c->sample_ms = (int)GetPrivateProfileIntW(CFG_SEC, L"sample_ms", c->sample_ms, wp);
   c->wave_buf = (int)GetPrivateProfileIntW(CFG_SEC, L"wave_buf", c->wave_buf, wp);
   c->last_flash_slave = (int)GetPrivateProfileIntW(CFG_SEC, L"last_flash_slave",
                                                     c->last_flash_slave, wp);
   c->lang = (int)GetPrivateProfileIntW(CFG_SEC, L"lang", c->lang, wp);

   /* 窗口：x,y,w,h,max 一次读完，格式不合法则保持默认 */
   if (GetPrivateProfileStringW(CFG_SEC, L"window", L"", buf, 512, wp) > 0)
   {
      int x = 0, y = 0, w = 0, h = 0, mx = 0;
      if (swscanf(buf, L"%d,%d,%d,%d,%d", &x, &y, &w, &h, &mx) == 5)
      {
         c->win_x = x;
         c->win_y = y;
         c->win_w = w;
         c->win_h = h;
         c->win_max = (mx != 0);
      }
   }

   buf[0] = L'\0';
   if (GetPrivateProfileStringW(CFG_SEC, L"last_adapter", L"", buf, 512, wp) > 0 &&
       WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), NULL, NULL) > 0)
   {
      strncpy(c->last_adapter, utf8, sizeof(c->last_adapter) - 1);
      c->last_adapter[sizeof(c->last_adapter) - 1] = '\0';
   }

   /* 合法性校验（手改 ini 或旧版本遗留的非法值一律回落默认） */
   if (c->skin < 1 || c->skin > 3)
      c->skin = 1;
   if (c->slave_mode != 0 && c->slave_mode != 1)
      c->slave_mode = -1;
   if (c->sample_ms != 1 && c->sample_ms != 5 && c->sample_ms != 10 &&
       c->sample_ms != 50 && c->sample_ms != 100 &&
       c->sample_ms != 200 && c->sample_ms != 500)
      c->sample_ms = 100;
   if (c->wave_buf < 1000 || c->wave_buf > 1000000)
      c->wave_buf = 100000;
   if (c->win_w < 0) c->win_w = 0;
   if (c->win_h < 0) c->win_h = 0;
   if (c->last_flash_slave < 1) c->last_flash_slave = 1;
   if (c->lang != 0 && c->lang != 1)
      c->lang = 0;
   return 1;
}

void ui_config_save(const ui_config_t *c)
{
   wchar_t wp[MAX_PATH];
   wchar_t num[32];
   wchar_t buf[512];

   if (c == NULL)
      return;
   if (!cfg_wide_path(wp, MAX_PATH))
      return;

   swprintf(num, 32, L"%d", c->skin);
   WritePrivateProfileStringW(CFG_SEC, L"skin", num, wp);
   swprintf(num, 32, L"%d", c->slave_mode);
   WritePrivateProfileStringW(CFG_SEC, L"slave_mode", num, wp);
   swprintf(num, 32, L"%d", c->sample_ms);
   WritePrivateProfileStringW(CFG_SEC, L"sample_ms", num, wp);
   swprintf(num, 32, L"%d", c->wave_buf);
   WritePrivateProfileStringW(CFG_SEC, L"wave_buf", num, wp);
   swprintf(num, 32, L"%d", c->last_flash_slave);
   WritePrivateProfileStringW(CFG_SEC, L"last_flash_slave", num, wp);
   swprintf(num, 32, L"%d", c->lang);
   WritePrivateProfileStringW(CFG_SEC, L"lang", num, wp);

   swprintf(buf, 512, L"%d,%d,%d,%d,%d",
            c->win_x, c->win_y, c->win_w, c->win_h, c->win_max ? 1 : 0);
   WritePrivateProfileStringW(CFG_SEC, L"window", buf, wp);

   if (c->last_adapter[0] != '\0')
   {
      if (MultiByteToWideChar(CP_UTF8, 0, c->last_adapter, -1, buf, 512) > 0)
         WritePrivateProfileStringW(CFG_SEC, L"last_adapter", buf, wp);
   }
   else
      WritePrivateProfileStringW(CFG_SEC, L"last_adapter", L"", wp);
}
