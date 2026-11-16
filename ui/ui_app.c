/*
 * ui_app.c - SimpleCAT 图形界面（Win32 API + GDI 基础绘图，无图片资源）
 *
 * 界面布局（对照设计草图 2026-11-10）：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ SimpleCAT V1.0   菜单  设置  皮肤  软件信息                   │  ← 顶栏
 *   │ ┌──────┐ ┌──────────────────────────────┐ ┌───────────────┐ │
 *   │ │操作  │ │                              │ │从站变量树     │ │
 *   │ │扫描  │ │      实时波形区              │ │ Slave1        │ │
 *   │ │烧录  │ │ （多变量叠加，VOFA+ 风格：  │ │  Output:      │ │
 *   │ │模式  │ │  X 轴时间 ms / Y 轴量程，   │ │   变量1   0   │ │
 *   │ │────  │ │  滚轮缩放 / 拖动平移）      │ │  Input:       │ │
 *   │ │从站  │ │                              │ │   变量1  11   │ │
 *   │ │状态  │ └──────────────────────────────┘ └───────────────┘ │
 *   │ └──────┘ ┌──────────────────────────────┐                   │
 *   │          │ 消息（时间戳，可清空）       │                   │
 *   │          └──────────────────────────────┘                   │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 交互约定：
 *   - 右侧树：单击输出变量的数值框 -> 就地编辑并下发；
 *     单击输入变量行 -> 加入/移出波形叠加（最多 8 条，自动配色）；
 *   - 波形区：滚轮缩放时间轴，Ctrl+滚轮缩放 Y 轴，
 *     按住左键拖动平移，双击恢复"跟随最新 + 自动量程"；
 *   - 所有控件均为 GDI 基础绘图；弹出菜单使用系统 TrackPopupMenu。
 */

#include "ui_app.h"
#include "ui_master.h"
#include "ui_config.h"
#include "esi_flash.h"
#include "ui_lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <shellapi.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>   /* timeBeginPeriod：1ms 采样线程的睡眠精度 */

/* ============================ 常量 ============================ */

#define APP_CLASS   L"SimpleCATUI"
#define APP_TITLE   L"SimpleCAT V1.0"
#define WIN_W       1280
#define WIN_H       800
#define MIN_W       1024
#define MIN_H       680

#define TOP_H       34      /* 顶栏高度 */
#define LEFT_W      176     /* 左侧操作面板宽度（基准 WIN_W 时的宽度） */
#define RIGHT_W     240     /* 右侧从站变量树宽度（基准 WIN_W 时的宽度） */
#define LEFT_W_MIN  150     /* 左面板最小宽度 */
#define LEFT_W_MAX  360     /* 左面板最大宽度 */
#define RIGHT_W_MIN 210     /* 右树最小宽度 */
#define RIGHT_W_MAX 560     /* 右树最大宽度 */
#define LOG_H       150     /* 底部消息区高度 */
#define GAP         8

#define TREE_ROW_H  22      /* 右侧树行高（15px 字体行高约 20px，需留出下伸部） */
#define LOG_LINE_H  17      /* 消息行高 */

/* 每通道缓冲区上限（点数，VOFA+ 风格可调）：1000 ~ 1000000 */
#define WAVE_CAP_MIN     1000
#define WAVE_CAP_MAX     1000000
#define WAVE_CAP_DEFAULT 100000
#define CHAN_MAX    8       /* 最多同时叠加的波形通道数 */
#define LOG_MAX     400     /* 消息日志最大条数 */

/* ============================ 主题（皮肤） ============================ */

typedef struct
{
   const char *name;
   COLORREF bg, panel, border, text, text_dim, accent, ok, warn, err;
   COLORREF head, sel, hover, btnface, wavegrid, wavebg, valbox;
} theme_t;

static const theme_t TH_LIGHT =
{
   "浅色",
   RGB(238,240,244), RGB(255,255,255), RGB(203,207,216), RGB(32,32,32),
   RGB(122,126,136), RGB(0,110,200), RGB(16,124,16), RGB(176,110,0),
   RGB(196,43,28), RGB(246,247,250), RGB(219,234,252), RGB(232,240,252),
   RGB(245,246,249), RGB(235,238,242), RGB(252,252,253), RGB(244,246,250)
};

static const theme_t TH_DARK =
{
   "深色",
   RGB(32,32,36), RGB(45,45,50), RGB(78,80,90), RGB(226,226,226),
   RGB(150,152,160), RGB(80,160,255), RGB(80,200,90), RGB(230,170,60),
   RGB(235,90,80), RGB(56,56,62), RGB(50,80,120), RGB(60,66,80),
   RGB(56,58,66), RGB(58,62,72), RGB(38,38,44), RGB(52,56,66)
};

static const theme_t TH_BLUE =
{
   "蓝灰",
   RGB(226,234,242), RGB(248,251,254), RGB(160,180,205), RGB(25,40,60),
   RGB(105,125,150), RGB(0,105,180), RGB(16,124,16), RGB(190,110,0),
   RGB(196,43,28), RGB(238,243,250), RGB(210,228,250), RGB(222,234,248),
   RGB(240,245,251), RGB(222,232,242), RGB(250,252,255), RGB(238,243,250)
};

static const theme_t *g_th = &TH_LIGHT;

/* 波形通道固定配色（VOFA+ 风格高饱和） */
static const COLORREF CHAN_COLORS[CHAN_MAX] =
{
   RGB(0,120,215), RGB(216,57,52), RGB(16,124,16), RGB(240,150,10),
   RGB(136,23,152), RGB(0,153,188), RGB(107,105,214), RGB(160,80,0)
};

/* ============================ 菜单/按钮 ID ============================ */

#define IDM_MAIN      1001     /* 顶栏-菜单 */
#define IDM_SET       1002     /* 顶栏-设置 */
#define IDM_SKIN      1003     /* 顶栏-皮肤 */
#define IDM_ABOUTBTN  1004     /* 顶栏-软件信息 */

#define IDM_NIC       1101     /* 菜单-选择网卡 */
#define IDM_FLASH     1103     /* 菜单-烧录 XML */
#define IDM_OPXMLDIR  1105     /* 菜单-打开 xml 目录 */
#define IDM_EXIT      1104

#define IDM_DC        1201
#define IDM_FREERUN   1202
#define IDM_SAMP_BASE 1210     /* 设置-采样周期 1211..1214 */

#define IDM_SKIN_BASE 1300     /* 皮肤 1301..1303 */
#define IDM_ABOUT     1305

#define IDM_LANG_BASE 1400     /* 设置-语言 1401=中文 1402=English */

#define IDM_ADAPTER_BASE 2000  /* 网卡弹窗菜单项基址 */
#define IDM_XML_BASE     3000  /* 烧录文件弹窗菜单项基址 */

/* 左侧面板自绘按钮 */
#define BTN_FLASH     1
#define BTN_RESCAN    2
#define BTN_MODE      6
#define BTN_CLEARLOG  7

/* ============================ 波形通道 ============================ */

typedef struct
{
   int var;                 /* 变量表下标 */
   long long tick0;         /* 建通道时的采样 tick（时间对齐用） */
   double *buf;             /* 环形缓冲（动态分配，容量 g_wave_cap），下标 t % g_wave_cap */
} wchan_t;

static wchan_t g_chan[CHAN_MAX];
static int g_chan_count = 0;
static long long g_tick_total = 0;   /* 已采样 tick 总数 */
static CRITICAL_SECTION g_wave_cs;   /* 保护 g_chan/g_tick_total（采样线程 <-> 界面线程） */
static int g_wave_cap = WAVE_CAP_DEFAULT;   /* 每通道缓冲区上限（点数） */

/* 采样周期与视图状态 */
static int g_sample_ms = 100;        /* 1/5/10/50/100/200/500 */
static int g_follow = 1;             /* 跟随最新 */
static double g_span_ms = 10000.0;   /* X 轴显示窗口（ms） */
static double g_right_age = 0.0;     /* 右边缘相对最新样本的年龄（ms） */
static int g_ymode_auto = 1;         /* Y 轴自动量程 */
static double g_ylo = -1.0, g_yhi = 1.0;

static int g_dragging = 0, g_drag_x = 0, g_drag_y = 0;
static int g_paused = 0;             /* 1 = 界面暂停刷新（快照/波形/日志冻结，主站通信继续） */

/* 波形区鼠标游标（VOFA+ 风格：悬停显示该时刻各通道数值） */
static int g_wave_hover = 0;
static int g_wave_mx = 0, g_wave_my = 0;

/* ============================ 消息日志 ============================ */

typedef struct
{
   char t[16];      /* HH:MM:SS.mmm */
   char text[200];
   int  err;        /* 1 = 报错消息（消息栏红色显示） */
} logline_t;

static logline_t g_log[LOG_MAX];
static int g_log_count = 0, g_log_head = 0;
static int g_log_scroll = 0;         /* 0 = 跟随最新（底部） */
static int g_log_sel_a = -1;         /* 拖选锚点行（绝对索引，0 = 最旧；-1 = 无选择） */
static int g_log_sel_ac = 0;         /* 锚点行内字符列号（宽字符下标） */
static int g_log_sel_b = -1;         /* 拖选终点行（绝对索引） */
static int g_log_sel_bc = 0;         /* 终点行内字符列号 */
static int g_log_selecting = 0;      /* 1 = 鼠标正在拖选消息 */
static unsigned g_last_msg_seq = 0;  /* 已拉取的主站消息序号 */

/* 错误计数：消息栏表头右侧显示 "ERR: N"（点击"清空"归零） */
static int g_err_count = 0;

/* 判定消息是否为报错（用于消息栏红色显示）：中文关键字 + 英文关键字（忽略大小写） */
static int msg_is_error(const char *s)
{
   static const char *const kw_zh[] = {
      "失败", "错误", "异常", "报错", "不符", "无法", "不可", "失联",
      "中止", "超时", "不在 OP", NULL
   };
   static const char *const kw_en[] = {
      "fail", "error", "mismatch", "invalid", "cannot", "unable",
      "lost", "abort", "timeout", "exceed", NULL
   };
   char low[200];
   int i, n;

   if (s == NULL || s[0] == '\0') return 0;
   for (i = 0; kw_zh[i] != NULL; ++i)
      if (strstr(s, kw_zh[i]) != NULL) return 1;

   /* 英文关键字按小写匹配（仅转 ASCII 部分） */
   n = (int)strlen(s);
   if (n >= (int)sizeof(low)) n = (int)sizeof(low) - 1;
   for (i = 0; i < n; ++i)
      low[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
   low[n] = '\0';
   for (i = 0; kw_en[i] != NULL; ++i)
      if (strstr(low, kw_en[i]) != NULL) return 1;
   return 0;
}

static void log_push_ex(const char *text, int err)
{
   logline_t *l;
   SYSTEMTIME st;
   GetLocalTime(&st);
   l = &g_log[g_log_head];
   snprintf(l->t, sizeof(l->t), "%02u:%02u:%02u.%03u",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
   snprintf(l->text, sizeof(l->text), "%s", text);
   l->err = err ? 1 : 0;
   if (l->err) g_err_count++;        /* 错误计数：消息栏表头显示 ERR: N */
   g_log_head = (g_log_head + 1) % LOG_MAX;
   if (g_log_count < LOG_MAX) g_log_count++;
   else
   {
      /* 环形缓冲已满：最旧一条被覆盖，绝对索引整体前移一位，
         已选中的行同步前移，否则高亮会错行 */
      if (g_log_sel_a >= 0) g_log_sel_a--;
      if (g_log_sel_b >= 0) g_log_sel_b--;
      if (g_log_sel_a < 0 || g_log_sel_b < 0)
         g_log_sel_a = g_log_sel_b = -1;
   }
   if (g_log_scroll == 0) /* 跟随模式：新消息可见 */ ;
   else g_log_scroll--;   /* 回看时保持视窗相对位置 */
}

static void log_push(const char *text)
{
   log_push_ex(text, 0);
}

static const logline_t *log_get(int idx) /* idx: 0 = 最旧 */
{
   int start = (g_log_head - g_log_count + LOG_MAX) % LOG_MAX;
   return &g_log[(start + idx) % LOG_MAX];
}

/* ============================ 全局状态 ============================ */

static HWND g_hwnd;
static HFONT g_font, g_font_small, g_font_bold, g_font_title;
static ui_snap_t g_snap;             /* 定时刷新的快照 */
static int g_flash_wait_shown = 0;
static int g_last_flash_seq = 0;

/* 右侧树状态 */
static int g_slave_exp[MASTER_MAXSLAVE + 1];   /* 从站展开状态 */
static int g_sec_exp[MASTER_MAXSLAVE + 1][2];  /* Output/Input 节展开状态（1=展开） */
static int g_tree_scroll = 0;

/* 左侧 CiA402 驱动控制区状态 */
static int g_c402_open = 1;      /* 1 = 展开 */
static int g_c402_sel = 0;       /* 当前显示的从站号（0 = 自动取第一个带 CiA402 的从站） */
static int g_hover_val_var = -1;               /* 悬停的输出变量数值框 */
static int g_sb_drag = 0;                      /* 1 = 正在拖动变量树滚动条滑块 */
static int g_sb_grab = 0;                      /* 按下点相对滑块顶部的偏移 */

static void tree_reset_expand(void)
{
   memset(g_slave_exp, 0, sizeof(g_slave_exp));
   memset(g_sec_exp, 1, sizeof(g_sec_exp));    /* 节默认展开 */
}

/* 输出变量就地编辑 */
static HWND g_edit_val;
static WNDPROC g_edit_oldproc;

/* 波形缓冲区上限设置（VOFA+ 风格，波形面板底部） */
static HWND g_edit_cap;                /* 数字编辑框 */
static WNDPROC g_cap_oldproc;
static RECT g_cap_er;                  /* 上次定位矩形（变了才 MoveWindow） */
static int g_cap_placed = 0;
static int g_editing_idx = -1;                 /* 正在编辑的变量下标 */
static DWORD g_edit_begin_tick = 0;            /* 进入编辑的时刻（双击保护窗口用） */

/* 双击进入编辑时，系统还会补发第二次 WM_LBUTTONDOWN；该按下不应取消编辑 */
static int edit_in_dblclk_guard(void)
{
   return (GetTickCount() - g_edit_begin_tick) < (DWORD)GetDoubleClickTime();
}

/* ============================ 配置记忆（SimpleCAT.ini） ============================ */

static ui_config_t g_cfg;                      /* 启动时读入，改动时回写 */

static int theme_id_of(const theme_t *th)
{
   if (th == &TH_DARK) return 2;
   if (th == &TH_BLUE) return 3;
   return 1;
}

static const theme_t *theme_from_id(int id)
{
   if (id == 2) return &TH_DARK;
   if (id == 3) return &TH_BLUE;
   return &TH_LIGHT;
}

/* 收集当前界面/主站偏好并写盘（窗口几何取"还原后"的矩形） */
static void config_save_now(void)
{
   if (g_hwnd != NULL)
   {
      WINDOWPLACEMENT wp;
      memset(&wp, 0, sizeof(wp));
      wp.length = sizeof(wp);
      if (GetWindowPlacement(g_hwnd, &wp))
      {
         RECT r = wp.rcNormalPosition;
         g_cfg.win_x = r.left;
         g_cfg.win_y = r.top;
         g_cfg.win_w = r.right - r.left;
         g_cfg.win_h = r.bottom - r.top;
         g_cfg.win_max = (wp.showCmd == SW_SHOWMAXIMIZED);
      }
   }
   g_cfg.skin = theme_id_of(g_th);
   g_cfg.sample_ms = g_sample_ms;
   g_cfg.slave_mode = ui_master_get_mode_pref();
   ui_config_save(&g_cfg);
}

/* ============================ 小工具 ============================ */

static int u2w(const char *s, wchar_t *w, int wn)
{
   return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
}

static void fill_rect_c(HDC hdc, RECT rc, COLORREF c)
{
   HBRUSH b = CreateSolidBrush(c);
   FillRect(hdc, &rc, b);
   DeleteObject(b);
}

static void frame_rect_c(HDC hdc, RECT rc, COLORREF c)
{
   HBRUSH b = CreateSolidBrush(c);
   FrameRect(hdc, &rc, b);
   DeleteObject(b);
}

static int pt_in(RECT rc, POINT p)
{
   return p.x >= rc.left && p.x < rc.right && p.y >= rc.top && p.y < rc.bottom;
}

static void text_out(HDC hdc, RECT rc, const char *s, COLORREF c, UINT fmt, HFONT f)
{
   wchar_t w[256];
   HFONT oft = NULL;
   u2w(s, w, 256);
   if (f) oft = (HFONT)SelectObject(hdc, f);   /* 用完恢复，避免粗体等残留在 DC 里 */
   SetTextColor(hdc, c);
   SetBkMode(hdc, TRANSPARENT);
   DrawTextW(hdc, w, -1, &rc, fmt);
   if (oft) SelectObject(hdc, oft);
}

/* 面板：底色 + 边框 + 左上角标题 */
static void draw_panel(HDC hdc, RECT rc, const char *title)
{
   RECT tr;
   fill_rect_c(hdc, rc, g_th->panel);
   frame_rect_c(hdc, rc, g_th->border);
   if (title == NULL) return;
   tr = rc;
   tr.left += 8; tr.top += 3; tr.bottom = tr.top + 20; tr.right -= 64;
   text_out(hdc, tr, title, g_th->accent, DT_LEFT | DT_SINGLELINE, g_font_bold);
}

/* 自绘按钮 */
static void draw_button(HDC hdc, RECT rc, const char *label,
                        int hover, int down, int enabled, int checked)
{
   wchar_t wl[64];
   RECT tr = rc;
   COLORREF bg = checked ? g_th->sel : g_th->btnface;
   if (!enabled) bg = g_th->head;
   else if (down) bg = g_th->sel;
   else if (hover) bg = g_th->hover;

   u2w(label, wl, 64);
   fill_rect_c(hdc, rc, bg);
   frame_rect_c(hdc, rc, enabled ? g_th->border : g_th->head);
   SetTextColor(hdc, enabled ? g_th->text : g_th->text_dim);
   SetBkMode(hdc, TRANSPARENT);
   DrawTextW(hdc, wl, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 从站状态对应的指示颜色 */
static COLORREF state_color(const ui_slave_info_t *s)
{
   if (s->islost) return g_th->err;
   if (s->state == EC_STATE_OPERATIONAL) return g_th->ok;
   if (s->state == EC_STATE_SAFE_OP || s->state == EC_STATE_PRE_OP) return g_th->warn;
   return g_th->err;
}

/* 弹出菜单辅助：构建后 TrackPopupMenu，返回选中项（0=取消） */
static int popup_menu(HMENU menu, HWND hwnd, POINT p)
{
   static int busy = 0;
   int id;
   if (busy) { DestroyMenu(menu); return 0; }
   busy = 1;
   SetForegroundWindow(hwnd);
   id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                       p.x, p.y, 0, hwnd, NULL);
   DestroyMenu(menu);
   PostMessageW(hwnd, WM_NULL, 0, 0);  /* MSDN 建议，确保菜单正确释放 */
   busy = 0;
   return id;
}

static HMENU new_popup(void) { return CreatePopupMenu(); }

static void menu_item_utf8(HMENU m, UINT id, const char *text, UINT flags)
{
   wchar_t w[256];
   u2w(text, w, 256);
   AppendMenuW(m, MF_STRING | flags, id, w);
}

static int msgbox_utf8(const char *text, const char *caption, UINT type)
{
   wchar_t wt[512], wc[128];
   u2w(text, wt, 512);
   u2w(caption, wc, 128);
   return MessageBoxW(g_hwnd, wt, wc, type);
}

/* 好看的坐标刻度步长 */
static double nice_step(double raw)
{
   double mag, n;
   if (raw <= 0) return 1.0;
   mag = pow(10.0, floor(log10(raw)));
   n = raw / mag;
   return ((n < 1.5) ? 1.0 : (n < 3.5) ? 2.0 : (n < 7.5) ? 5.0 : 10.0) * mag;
}

/* ============================ 布局 ============================ */

typedef struct
{
   RECT top, left, wave, log, right;
} layout_t;

static void layout_calc(layout_t *L, int W, int H)
{
   /* 左右面板宽度随窗口宽度等比拉伸（以设计基准 WIN_W 为 100%）：
      拉动窗口边缘时所有功能区整体变宽，而不是只有波形区。
      限幅防止过窄挤压内容或过宽浪费空间 */
   int lw = LEFT_W * W / WIN_W;
   int rw = RIGHT_W * W / WIN_W;

   if (lw < LEFT_W_MIN) lw = LEFT_W_MIN;
   if (lw > LEFT_W_MAX) lw = LEFT_W_MAX;
   if (rw < RIGHT_W_MIN) rw = RIGHT_W_MIN;
   if (rw > RIGHT_W_MAX) rw = RIGHT_W_MAX;

   L->top.left = 0;            L->top.top = 0;
   L->top.right = W;           L->top.bottom = TOP_H;

   L->left.left = GAP;         L->left.top = TOP_H + GAP;
   L->left.right = GAP + lw;   L->left.bottom = H - GAP;

   L->right.left = W - GAP - rw; L->right.top = TOP_H + GAP;
   L->right.right = W - GAP;     L->right.bottom = H - GAP;

   L->wave.left = GAP + lw + GAP; L->wave.top = TOP_H + GAP;
   L->wave.right = W - GAP - rw - GAP;
   L->wave.bottom = H - GAP - LOG_H - GAP;

   L->log.left = GAP + lw + GAP;  L->log.top = L->wave.bottom + GAP;
   L->log.right = W - GAP - rw - GAP; L->log.bottom = H - GAP;
}

/* 左侧四个按钮的矩形（绘制与命中共用） */
static void left_buttons(const RECT *rc, RECT *b_scan, RECT *b_flash,
                         RECT *b_mode, RECT *b_pause)
{
   int x = rc->left + 10, w = rc->right - rc->left - 20, h = 30;
   b_scan->left = x;          b_scan->top = rc->top + 28;
   b_scan->right = x + w;     b_scan->bottom = b_scan->top + h;
   b_flash->left = x;         b_flash->top = b_scan->bottom + 8;
   b_flash->right = x + w;    b_flash->bottom = b_flash->top + h;
   b_mode->left = x;          b_mode->top = b_flash->bottom + 8;
   b_mode->right = x + w;     b_mode->bottom = b_mode->top + h;
   b_pause->left = x;         b_pause->top = b_mode->bottom + 8;
   b_pause->right = x + w;    b_pause->bottom = b_pause->top + h;
}

/* 消息面板"清空"按钮矩形 */
static RECT log_clear_rect(const RECT *rc)
{
   RECT b;
   b.right = rc->right - 6; b.left = b.right - 46;
   b.top = rc->top + 3;     b.bottom = b.top + 18;
   return b;
}

/* ============================ 波形 ============================ */

/* 最新样本的年龄上限（ms） */
static double wave_max_age(void)
{
   return (g_tick_total > 0) ? (double)(g_tick_total - 1) * g_sample_ms : 0.0;
}

/* 通道在 tick t 的样本是否有效 */
static int chan_valid(const wchan_t *c, long long t)
{
   return t >= c->tick0 && t < g_tick_total;
}

static double chan_get(const wchan_t *c, long long t)
{
   if (c->buf == NULL) return 0.0;
   return c->buf[t % g_wave_cap];
}

/* 通道当前值（快照） */
static long long chan_cur_value(const wchan_t *c)
{
   if (c->var < 0 || c->var >= g_snap.vl.count) return 0;
   return (g_snap.vl.var[c->var].dir == 0) ? g_snap.oval[c->var]
                                           : g_snap.ival[c->var];
}

/* 确保通道有可用缓冲（缓冲随通道结构体一起前移，故可能为空） */
static int chan_ensure_buf(wchan_t *c)
{
   if (c->buf != NULL) return 1;
   c->buf = (double *)malloc((size_t)g_wave_cap * sizeof(double));
   return (c->buf != NULL);
}

/* 移除第 k 条通道。缓冲指针存在通道结构体里：
   - 后面的通道整体前移时，缓冲所有权随之转移，尾部残留副本必须置空（否则会重复释放）
   - 删除的是最后一条时，直接释放它的缓冲 */
static void chan_remove_at(int k)
{
   if (k < 0 || k >= g_chan_count) return;
   if (k < g_chan_count - 1)
   {
      int i;
      for (i = k; i < g_chan_count - 1; ++i) g_chan[i] = g_chan[i + 1];
      g_chan_count--;
      g_chan[g_chan_count].buf = NULL;
   }
   else
   {
      g_chan_count--;
      free(g_chan[g_chan_count].buf);
      memset(&g_chan[g_chan_count], 0, sizeof(g_chan[g_chan_count]));
   }
}

/* 加入/移出一条波形通道 */
static void chan_toggle(int var_index)
{
   int i, k = -1;
   EnterCriticalSection(&g_wave_cs);
   for (i = 0; i < g_chan_count; ++i)
      if (g_chan[i].var == var_index) { k = i; break; }

   if (k >= 0)   /* 已在 -> 移除 */
   {
      chan_remove_at(k);
      LeaveCriticalSection(&g_wave_cs);
      return;
   }
   if (g_chan_count >= CHAN_MAX)
   {
      LeaveCriticalSection(&g_wave_cs);
      msgbox_utf8(TR("最多同时叠加 8 条波形，请先点击已显示的变量将其移除。",
                     "At most 8 waveforms can be plotted; click a plotted variable first to remove it."),
                  TR("波形", "Waveform"), MB_ICONINFORMATION);
      return;
   }
   if (var_index < 0 || var_index >= g_snap.vl.count)
   {
      LeaveCriticalSection(&g_wave_cs);
      return;
   }
   {
      wchan_t *c = &g_chan[g_chan_count];
      /* 注意：绝不能 memset 整个结构体——buf 是动态分配的指针，
         清零后缓冲就丢了，采样线程会跳过写入、绘图读到全 0（表现为一条直线） */
      c->var = var_index;
      c->tick0 = g_tick_total;   /* 从当前 tick 开始积累 */
      if (!chan_ensure_buf(c))
      {
         log_push(TR("内存不足，无法新增波形通道", "Out of memory, cannot add waveform channel"));
         LeaveCriticalSection(&g_wave_cs);
         return;
      }
      g_chan_count++;
      if (g_tick_total == 0) g_follow = 1;
   }
   LeaveCriticalSection(&g_wave_cs);
}

/* 重扫/重连后清理全部通道 */
static void chan_clear_all(void)
{
   EnterCriticalSection(&g_wave_cs);
   g_chan_count = 0;
   g_tick_total = 0;
   g_follow = 1;
   g_right_age = 0;
   g_span_ms = 10000.0;
   g_ymode_auto = 1;
   LeaveCriticalSection(&g_wave_cs);
}

/* ============================ 波形采样线程 ============================ */

/* 1ms 精度独立采样：20ms 界面定时器太慢，采样周期 <20ms 时只能靠线程。
   每次循环直接从主站拉一份快照（主循环 ~1ms 更新一次过程数据，
   1ms 采样即逐周期取值）。缓冲区改动用 g_wave_cs 与界面线程互斥 */
static volatile LONG g_sample_quit = 0;
static HANDLE g_sample_thread = NULL;

/* 启动时为全部通道槽位分配缓冲（内存不足则逐级减半重试） */
static void wave_bufs_alloc(void)
{
   int i, cap = g_wave_cap;
   while (cap >= WAVE_CAP_MIN)
   {
      int ok = 1;
      for (i = 0; i < CHAN_MAX; ++i)
      {
         g_chan[i].buf = (double *)malloc((size_t)cap * sizeof(double));
         if (g_chan[i].buf == NULL) ok = 0;
      }
      if (ok)
      {
         g_wave_cap = cap;
         return;
      }
      for (i = 0; i < CHAN_MAX; ++i)
      {
         free(g_chan[i].buf);
         g_chan[i].buf = NULL;
      }
      cap /= 2;
   }
}

/* 应用新的缓冲区上限（主线程上下文）：重分配并清空波形数据 */
static void wave_cap_apply(int cap)
{
   int i;

   if (cap < WAVE_CAP_MIN) cap = WAVE_CAP_MIN;
   if (cap > WAVE_CAP_MAX) cap = WAVE_CAP_MAX;
   if (cap == g_wave_cap)
      return;

   EnterCriticalSection(&g_wave_cs);
   {
      int ok = 1;
      for (i = 0; i < CHAN_MAX; ++i)
      {
         double *nb = (double *)realloc(g_chan[i].buf, (size_t)cap * sizeof(double));
         if (nb != NULL) g_chan[i].buf = nb;
         else ok = 0;
      }
      if (ok)
      {
         g_wave_cap = cap;
         g_tick_total = 0;                       /* 容量变了，旧数据布局失效，全部清空 */
         for (i = 0; i < g_chan_count; ++i) g_chan[i].tick0 = 0;
         g_follow = 1;
         g_right_age = 0.0;
      }
   }
   LeaveCriticalSection(&g_wave_cs);

   if (g_wave_cap == cap)
   {
      char msg[96];
      g_cfg.wave_buf = cap;
      config_save_now();
      snprintf(msg, sizeof(msg), TR("波形缓冲区上限已设为 %d 点/通道（波形已清空）",
                                    "Wave buffer limit set to %d points/ch (waveform cleared)"), cap);
      log_push(msg);
   }
   else
      log_push(TR("内存不足，缓冲区上限设置失败", "Not enough memory, buffer limit unchanged"));
   InvalidateRect(g_hwnd, NULL, FALSE);
}

static DWORD WINAPI wave_sampler(LPVOID arg)
{
   ui_snap_t snap;
   (void)arg;
   timeBeginPeriod(1);
   while (!g_sample_quit)
   {
      int ms = g_sample_ms, c;
      Sleep(ms >= 1 ? ms : 1);
      if (g_paused) continue;   /* 暂停刷新：与界面冻结保持一致 */
      ui_master_get_snap(&snap);
#ifdef UI_TEST_TREE   /* 无硬件验证：与界面注入保持一致（快照值本身为 0） */
      if (snap.vl.count == 0)
      {
         int v;
         snap.vl.count = 60;
         for (v = 0; v < 60; ++v)
         {
            strcpy(snap.vl.var[v].name, "TestVar");
            snap.vl.var[v].index = 0x6000 + v / 4;
            snap.vl.var[v].dir = v % 2;
            snap.vl.var[v].slave = (uint8)(v % 3) + 1;
         }
      }
#ifdef UI_TEST_SINE   /* 端到端验证：模拟 RUNNING + 正弦过程数据（复现真实采样链路） */
      snap.mstate = MST_RUNNING;
      {
         int v;
         for (v = 0; v < 60; ++v)
         {
            long long ph = (long long)g_tick_total;
            long long s1 = (long long)(100.0 * sin(ph * 0.02) + 30.0 * sin(ph * 0.005));
            if (snap.vl.var[v].dir == 0) snap.oval[v] = s1;
            else                         snap.ival[v] = -s1;
         }
      }
#endif
#endif
      EnterCriticalSection(&g_wave_cs);
      if (g_chan_count > 0 && snap.mstate == MST_RUNNING)
      {
         for (c = 0; c < g_chan_count; ++c)
         {
            wchan_t *ch = &g_chan[c];
            if (ch->buf != NULL)
               ch->buf[g_tick_total % g_wave_cap] =
                  (ch->var < snap.vl.count)
                  ? (double)((snap.vl.var[ch->var].dir == 0)
                             ? snap.oval[ch->var] : snap.ival[ch->var])
                  : 0.0;
         }
         g_tick_total++;
      }
      LeaveCriticalSection(&g_wave_cs);
   }
   timeEndPeriod(1);
   return 0;
}

/* 波形面板"清空"按钮矩形（面板右上角，与消息区清空按钮同款式） */
static RECT wave_clear_rect(const RECT *rc)
{
   RECT b;
   b.right = rc->right - 8; b.left = b.right - 46;
   b.top = rc->top + 3;     b.bottom = b.top + 18;
   return b;
}

/* 波形绘图区（面板内部坐标） */
static RECT wave_plot_rect(const RECT *rc)
{
   RECT pr;
   pr.left = rc->left + 62;   /* Y 轴刻度标签 */
   pr.right = rc->right - 14;
   pr.top = rc->top + 28;     /* 图例 */
   pr.bottom = rc->bottom - 46; /* X 轴刻度标签 + 底部"缓冲区上限"设置行 */
   if (pr.bottom < pr.top + 40) pr.bottom = pr.top + 40;
   return pr;
}

static double age_to_x(double age, const RECT *pr)
{
   double w = pr->right - pr->left;
   return pr->right - (age - g_right_age) / g_span_ms * w;
}

static double x_to_age(double x, const RECT *pr)
{
   double w = pr->right - pr->left;
   return g_right_age + (pr->right - x) / w * g_span_ms;
}

/* 可见 t 范围（钳制到数据起点与环形缓冲有效历史内） */
static void wave_visible_t(long long *t_lo, long long *t_hi)
{
   double a_lo = g_right_age, a_hi = g_right_age + g_span_ms;
   (void)a_lo;
   if (g_tick_total <= 0) { *t_lo = 1; *t_hi = 0; return; }
   *t_hi = g_tick_total - 1 - (long long)(g_right_age / g_sample_ms);
   *t_lo = g_tick_total - 1 - (long long)((a_hi) / g_sample_ms) - 1;
   if (*t_hi > g_tick_total - 1) *t_hi = g_tick_total - 1;
   if (*t_lo < 0) *t_lo = 0;
   /* 关键：环形缓冲只保留最近 g_wave_cap 个样本，更老的 tick 槽位已被新数据
      覆盖；不钳制的话，老时间位置会画出新数据（表现为新旧波形冲突的突起） */
   if (*t_lo < g_tick_total - g_wave_cap) *t_lo = g_tick_total - g_wave_cap;
}

/* 自动 Y 量程（扫描可见样本） */
static void wave_auto_y(const RECT *pr, double *lo, double *hi)
{
   long long t_lo, t_hi, t;
   int c;
   double mn = 0, mx = 0;
   int have = 0;

   wave_visible_t(&t_lo, &t_hi);
   for (c = 0; c < g_chan_count; ++c)
   {
      long long t0 = (g_chan[c].tick0 > t_lo) ? g_chan[c].tick0 : t_lo;
      for (t = t0; t <= t_hi; ++t)
      {
         double s = chan_get(&g_chan[c], t);
         if (!have) { mn = mx = s; have = 1; }
         else { if (s < mn) mn = s; if (s > mx) mx = s; }
      }
   }
   if (!have)
   {
      /* 无样本：用当前值兜底 */
      for (c = 0; c < g_chan_count; ++c)
      {
         double s = (double)chan_cur_value(&g_chan[c]);
         if (!have) { mn = mx = s; have = 1; }
         else { if (s < mn) mn = s; if (s > mx) mx = s; }
      }
   }
   if (!have) { mn = 0; mx = 1; }
   if (mx - mn < 1e-9) { mn -= 1.0; mx += 1.0; }
   else { double pad = (mx - mn) * 0.15; mn -= pad; mx += pad; }
   *lo = mn; *hi = mx;
}

static void draw_wave(HDC hdc, const RECT *rc, POINT mp)
{
   RECT pr = wave_plot_rect(rc);
   double lo, hi;
   int c, i;
   wchar_t ws[160];

#ifdef UI_TEST_WAVE   /* 无硬件验证波形游标外观：cl -DUI_TEST_WAVE */
   /* 注意：定时器每帧会用未连接快照（vl.count=0）清掉通道，所以每帧都重新断言 */
   {
      int k;
      strcpy(g_snap.vl.var[0].name, "Test_CH1_sine");
      g_snap.vl.count = 1;
      if (g_chan_count == 0)
         chan_toggle(0);            /* 走真实添加路径，覆盖缓冲分配逻辑 */
      chan_ensure_buf(&g_chan[0]);
#ifndef UI_TEST_SINE   /* 端到端模式：数据由采样线程真实写入，这里不代填 */
      g_chan[0].tick0 = 0;
      g_sample_ms = 1;             /* 与真实 1ms 采样一致，复现 10s 窗口 vs 缓冲深度 */
      g_tick_total = g_wave_cap;
      if (g_chan[0].buf != NULL)
         for (k = 0; k < g_wave_cap; ++k)
            g_chan[0].buf[k] = 50.0 * sin(k * 0.05) + 15.0 * sin(k * 0.013);
#endif
      g_wave_hover = 1;
      g_wave_mx = (pr.left + pr.right) / 2;
      g_wave_my = (pr.top + pr.bottom) / 2;
   }
#endif

   draw_panel(hdc, *rc, TR("实时波形", "Live Waveform"));

   /* 底部：缓冲区上限设置行（VOFA+ 风格）："缓冲区上限: [100000] /CH"
      右侧 "/CH" 按实际文字宽度从面板右缘反推，任何 DPI 下都不会被裁掉 */
   {
      RECT er, lr, ur;
      wchar_t wv[32];
      SIZE usz;
      HFONT oft = (HFONT)SelectObject(hdc, g_font_small);
      GetTextExtentPoint32W(hdc, L"/CH", 3, &usz);
      SelectObject(hdc, oft);
      (void)oft;

      er.right = rc->right - usz.cx - 12;  /* 右侧留 "/CH" 文字实测宽度 */
      er.left = er.right - 76;
      er.top = rc->bottom - 26;
      er.bottom = rc->bottom - 6;

      lr = *rc; lr.right = er.left - 6; lr.top = er.top; lr.bottom = er.bottom;
      text_out(hdc, lr, TR("缓冲区上限:", "Buffer limit:"),
               g_th->text_dim, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, g_font_small);

      ur = *rc; ur.left = er.right + 6; ur.top = er.top; ur.bottom = er.bottom;
      text_out(hdc, ur, "/CH", g_th->text_dim, DT_LEFT | DT_SINGLELINE | DT_VCENTER, g_font_small);

      if (!g_cap_placed || !EqualRect(&er, &g_cap_er))
      {
         MoveWindow(g_edit_cap, er.left, er.top,
                    er.right - er.left, er.bottom - er.top, TRUE);
         g_cap_er = er;
         g_cap_placed = 1;
         if (GetWindowTextLengthW(g_edit_cap) == 0)
         {
            _snwprintf(wv, 32, L"%d", g_wave_cap);
            wv[31] = L'\0';
            SetWindowTextW(g_edit_cap, wv);
         }
      }
      ShowWindow(g_edit_cap, SW_SHOW);
   }

   if (g_chan_count == 0)
   {
      RECT tr = *rc;
      text_out(hdc, tr, TR("在右侧右键点击输入/输出变量，将其波形叠加到这里（支持多变量）",
                           "Right-click input/output variables on the right to plot them here (multiple supported)"),
               g_th->text_dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);
      return;
   }

   /* Y 量程 */
   if (g_ymode_auto) wave_auto_y(&pr, &lo, &hi);
   else { lo = g_ylo; hi = g_yhi; }
   if (hi - lo < 1e-9) { lo -= 1.0; hi += 1.0; }

   fill_rect_c(hdc, pr, g_th->wavebg);
   frame_rect_c(hdc, pr, g_th->border);

   /* Y 网格 + 刻度 */
   {
      double step = nice_step((hi - lo) / 5.0);
      double v;
      HPEN pen = CreatePen(PS_SOLID, 1, g_th->wavegrid);
      HPEN op = (HPEN)SelectObject(hdc, pen);
      char lab[48];
      double first = ceil(lo / step) * step;
      for (v = first; v <= hi + 1e-12; v += step)
      {
         int y = pr.bottom - (int)((v - lo) / (hi - lo) * (pr.bottom - pr.top));
         if (y < pr.top || y > pr.bottom) continue;
         MoveToEx(hdc, pr.left, y, NULL);
         LineTo(hdc, pr.right, y);
         snprintf(lab, sizeof(lab), "%.6g", v);
         {
            RECT lr = { rc->left + 4, y - 8, pr.left - 4, y + 8 };
            text_out(hdc, lr, lab, g_th->text_dim, DT_RIGHT | DT_SINGLELINE, g_font_small);
         }
      }
      SelectObject(hdc, op);
      DeleteObject(pen);
   }

   /* X 网格 + 刻度（绝对时间 ms） */
   {
      double step = nice_step(g_span_ms / 6.0);
      double t_right_abs = wave_max_age() - g_right_age;      /* 右边缘绝对 ms */
      double t_left_abs = t_right_abs - g_span_ms;
      double k, first;
      HPEN pen = CreatePen(PS_SOLID, 1, g_th->wavegrid);
      HPEN op = (HPEN)SelectObject(hdc, pen);
      char lab[48];

      first = ceil(t_left_abs / step) * step;
      for (k = first; k <= t_right_abs + 1e-9; k += step)
      {
         double age = wave_max_age() - k;
         int x = (int)age_to_x(age, &pr);
         if (x < pr.left || x > pr.right) continue;
         MoveToEx(hdc, x, pr.top, NULL);
         LineTo(hdc, x, pr.bottom);
         snprintf(lab, sizeof(lab), "%.0f", k);
         {
            RECT lr = { x - 40, pr.bottom + 2, x + 40, pr.bottom + 16 };
            text_out(hdc, lr, lab, g_th->text_dim, DT_CENTER | DT_SINGLELINE, g_font_small);
         }
      }
      {
         RECT lr = { pr.right - 70, pr.bottom + 4, pr.right, pr.bottom + 18 };
         text_out(hdc, lr, "ms", g_th->text_dim, DT_RIGHT | DT_SINGLELINE, g_font_small);
      }
      SelectObject(hdc, op);
      DeleteObject(pen);
   }

   /* 各通道折线 */
   for (c = 0; c < g_chan_count; ++c)
   {
      long long t, t_lo, t_hi, t0;
      HPEN pen = CreatePen(PS_SOLID, 2, CHAN_COLORS[c % CHAN_MAX]);
      HPEN op = (HPEN)SelectObject(hdc, pen);
      int started = 0;

      wave_visible_t(&t_lo, &t_hi);
      t0 = (g_chan[c].tick0 > t_lo) ? g_chan[c].tick0 : t_lo;
      for (t = t0; t <= t_hi; ++t)
      {
         double s = chan_get(&g_chan[c], t);
         double age = (double)(g_tick_total - 1 - t) * g_sample_ms;
         int x = (int)age_to_x(age, &pr);
         int y;
         if (x < pr.left - 2) continue;
         if (x > pr.right + 2) break;
         y = pr.bottom - (int)((s - lo) / (hi - lo) * (pr.bottom - pr.top));
         if (y < pr.top) y = pr.top;
         if (y > pr.bottom) y = pr.bottom;
         if (!started) { MoveToEx(hdc, x, y, NULL); started = 1; }
         else LineTo(hdc, x, y);
      }
      SelectObject(hdc, op);
      DeleteObject(pen);
   }
   (void)i;

   /* 图例 */
   {
      int lx = pr.left + 8, ly = rc->top + 4;
      for (c = 0; c < g_chan_count; ++c)
      {
         const tc3_variable_t *v;
         char item[192], cur[32];
         RECT box = { lx, ly + 3, lx + 12, ly + 13 };
         RECT tr;
         HBRUSH b;

         if (g_chan[c].var >= g_snap.vl.count) break;
         v = &g_snap.vl.var[g_chan[c].var];
         snprintf(cur, sizeof(cur), "%lld", chan_cur_value(&g_chan[c]));
         snprintf(item, sizeof(item), "%s = %s", v->name, cur);
         b = CreateSolidBrush(CHAN_COLORS[c % CHAN_MAX]);
         FillRect(hdc, &box, b);
         DeleteObject(b);
         tr.left = lx + 16; tr.right = lx + 16 + 220; tr.top = ly; tr.bottom = ly + 16;
         text_out(hdc, tr, item, g_th->text, DT_LEFT | DT_SINGLELINE, g_font_small);
         lx += 16 + 230;
         if (lx > pr.right - 240) { lx = pr.left + 8; ly += 16; }
      }
   }

   /* 右上角提示：视图状态 + 清空按钮 */
   {
      char tip[80];
      RECT tr = *rc; tr.right -= 12 + 46 + 6; tr.top += 4; tr.bottom = tr.top + 18;
      if (g_follow && g_ymode_auto) snprintf(tip, sizeof(tip), TR("跟随最新 · 自动量程", "Follow live · auto range"));
      else if (!g_follow && g_ymode_auto) snprintf(tip, sizeof(tip), TR("平移中（双击恢复）", "Panning (double-click to restore)"));
      else snprintf(tip, sizeof(tip), TR("手动量程 [%.4g, %.4g]", "Manual range [%.4g, %.4g]"), lo, hi);
      text_out(hdc, tr, tip, g_th->text_dim, DT_RIGHT | DT_SINGLELINE, g_font_small);

      {
         RECT clr = wave_clear_rect(rc);
         draw_button(hdc, clr, TR("清空", "Clear"), pt_in(clr, mp), 0, 1, 0);
      }
   }

   /* 鼠标测量游标（VOFA+ 风格）：竖直虚线 + 各通道该时刻圆点 + 数值浮框 */
   if (g_wave_hover && g_tick_total > 0 && g_chan_count > 0)
   {
      int cx = g_wave_mx, cy = g_wave_my;
      double age = x_to_age(cx, &pr);
      long long t = g_tick_total - 1 - (long long)(age / g_sample_ms);
      long long t_lo, t_hi;
      HPEN pen, op;

      if (t < 0) t = 0;
      wave_visible_t(&t_lo, &t_hi);
      if (t > t_hi) t = t_hi;
      if (t < t_lo) t = t_lo;

      /* 竖直点线 */
      pen = CreatePen(PS_DOT, 1, g_th->text_dim);
      op = (HPEN)SelectObject(hdc, pen);
      MoveToEx(hdc, cx, pr.top, NULL);
      LineTo(hdc, cx, pr.bottom);
      SelectObject(hdc, op);
      DeleteObject(pen);

      /* 各通道在该时刻的圆点标记 */
      for (c = 0; c < g_chan_count; ++c)
      {
         double s, xx;
         int xq, yq;
         HBRUSH b;

         if (g_chan[c].var >= g_snap.vl.count) continue;
         if (t < g_chan[c].tick0) continue;
         s = chan_get(&g_chan[c], t);
         xx = age_to_x((double)(g_tick_total - 1 - t) * g_sample_ms, &pr);
         xq = (int)xx; if (xq < pr.left) xq = pr.left;
         if (xq > pr.right) xq = pr.right;
         yq = pr.bottom - (int)((s - lo) / (hi - lo) * (pr.bottom - pr.top));
         if (yq < pr.top) yq = pr.top;
         if (yq > pr.bottom) yq = pr.bottom;
         b = CreateSolidBrush(CHAN_COLORS[c % CHAN_MAX]);
         {
            HBRUSH ob2 = (HBRUSH)SelectObject(hdc, b);
            HPEN fp = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, xq - 4, yq - 4, xq + 4, yq + 4);
            SelectObject(hdc, fp);
            SelectObject(hdc, ob2);
         }
         DeleteObject(b);
      }

      /* 数值浮框（跟在鼠标旁，超出右边缘时放到左侧） */
      {
         int k, n_line = 0, row = 0, maxlen = 0, bw, bh, bx, by;
         HFONT of = (HFONT)SelectObject(hdc, g_font_small);
         SIZE tsz;
         wchar_t wl[192];
         char line[192];

         for (k = 0; k < g_chan_count; ++k)
         {
            if (g_chan[k].var >= g_snap.vl.count) continue;
            if (t < g_chan[k].tick0) continue;
            n_line++;
            snprintf(line, sizeof(line), "%s = %lld",
                     g_snap.vl.var[g_chan[k].var].name,
                     (long long)(chan_get(&g_chan[k], t) < 0 ?
                                 chan_get(&g_chan[k], t) - 0.5 :
                                 chan_get(&g_chan[k], t) + 0.5));
            u2w(line, wl, 192);
            GetTextExtentPoint32W(hdc, wl, (int)wcslen(wl), &tsz);
            if (tsz.cx > maxlen) maxlen = tsz.cx;
         }

         if (n_line > 0)
         {
            bw = maxlen + 36;
            bh = 18 + n_line * 15 + 6;
            bx = cx + 14;
            if (bx + bw > pr.right - 2) bx = cx - 14 - bw;
            if (bx < pr.left + 2) bx = pr.left + 2;
            by = cy - bh / 2;
            if (by < pr.top + 2) by = pr.top + 2;
            if (by + bh > pr.bottom - 2) by = pr.bottom - 2 - bh;

            fill_rect_c(hdc, (RECT){ bx, by, bx + bw, by + bh }, g_th->panel);
            frame_rect_c(hdc, (RECT){ bx, by, bx + bw, by + bh }, g_th->border);

            /* 顶部：游标时刻（绝对 ms） */
            snprintf(line, sizeof(line), "%.0f ms",
                     wave_max_age() - (double)(g_tick_total - 1 - t) * g_sample_ms);
            {
               RECT tr2 = { bx + 8, by + 3, bx + bw - 4, by + 17 };
               text_out(hdc, tr2, line, g_th->text_dim, DT_LEFT | DT_SINGLELINE, g_font_small);
            }

            for (k = 0; k < g_chan_count; ++k)
            {
               int yy;
               RECT sq, tr2;
               if (g_chan[k].var >= g_snap.vl.count) continue;
               if (t < g_chan[k].tick0) continue;
               yy = by + 18 + row * 15;
               row++;
               snprintf(line, sizeof(line), "%s = %lld",
                        g_snap.vl.var[g_chan[k].var].name,
                        (long long)(chan_get(&g_chan[k], t) < 0 ?
                                    chan_get(&g_chan[k], t) - 0.5 :
                                    chan_get(&g_chan[k], t) + 0.5));
               sq.left = bx + 6; sq.right = bx + 14;
               sq.top = yy + 4; sq.bottom = yy + 12;
               fill_rect_c(hdc, sq, CHAN_COLORS[k % CHAN_MAX]);
               tr2.left = bx + 18; tr2.right = bx + bw - 4;
               tr2.top = yy; tr2.bottom = yy + 15;
               text_out(hdc, tr2, line, g_th->text, DT_LEFT | DT_SINGLELINE, g_font_small);
            }
         }
         SelectObject(hdc, of);
      }
   }
   (void)ws;
}

/* ============================ 右侧从站变量树 ============================ */

enum { RK_SLAVE = 1, RK_SECTION, RK_VAR, RK_ADDR };   /* RK_ADDR = CoE 对象地址分组行 */

typedef struct
{
   int kind, slave, var;
   RECT rc;         /* 整行 */
   RECT val;        /* 数值框（输出变量有效） */
} trow_t;

/* 构建可见行（含滚动裁剪），返回行数 */
static int tree_build(trow_t *rows, int maxrows, const RECT *rc)
{
   int n = 0, i, y = rc->top + 26;
   int bottom = rc->bottom - 44;   /* 底部留出两行操作提示，防变量行盖住文字 */
   int skip = g_tree_scroll;

   for (i = 1; i <= g_snap.slavecount; ++i)
   {
      int dir, v;
      if (y + 24 > bottom) break;   /* 从站头 22px：放不下就停（防半行压住提示文字） */

      /* 从站头 */
      if (skip > 0) skip--;
      else if (n < maxrows)
      {
         rows[n].kind = RK_SLAVE; rows[n].slave = i; rows[n].var = -1;
         rows[n].rc.left = rc->left + 4; rows[n].rc.right = rc->right - 4;
         rows[n].rc.top = y; rows[n].rc.bottom = y + 22;
         rows[n].val.left = rows[n].val.right = rows[n].val.top = rows[n].val.bottom = 0;
         n++; y += 22;
      }
      else y += 22;
      if (!g_slave_exp[i]) continue;

      for (dir = 0; dir <= 1; ++dir)
      {
         /* 节标题 */
         if (skip > 0) skip--;
         else if (n < maxrows && y + TREE_ROW_H <= bottom)
         {
            rows[n].kind = RK_SECTION; rows[n].slave = i; rows[n].var = dir;
            rows[n].rc.left = rc->left + 4; rows[n].rc.right = rc->right - 4;
            rows[n].rc.top = y; rows[n].rc.bottom = y + TREE_ROW_H;
            rows[n].val.left = rows[n].val.right = rows[n].val.top = rows[n].val.bottom = 0;
            n++; y += TREE_ROW_H;
         }
         else y += TREE_ROW_H;

         if (!g_sec_exp[i][dir]) continue;   /* 节收起：跳过下面的变量 */

         {
            int last_idx = -1;   /* 当前地址分组的 CoE 索引（-1 = 还没有） */
            for (v = 0; v < g_snap.vl.count; ++v)
            {
               const tc3_variable_t *var = &g_snap.vl.var[v];
               if (var->dir != dir) continue;
               if (var->slave != 0 && var->slave != i) continue;

               /* 地址分组行：CoE 索引变化时插入一行（红色 0xXXXX） */
               if ((int)var->index != last_idx)
               {
                  last_idx = var->index;
                  if (skip > 0) { skip--; }   /* 视口上方：不推进 y */
                  else if (n < maxrows && y + TREE_ROW_H <= bottom)
                  {
                     rows[n].kind = RK_ADDR; rows[n].slave = i; rows[n].var = v;
                     rows[n].rc.left = rc->left + 4; rows[n].rc.right = rc->right - 4;
                     rows[n].rc.top = y; rows[n].rc.bottom = y + TREE_ROW_H;
                     rows[n].val.left = rows[n].val.right = rows[n].val.top = rows[n].val.bottom = 0;
                     n++;
                     y += TREE_ROW_H;
                  }
                  else
                     y += TREE_ROW_H;   /* 放不下：仍消耗行高 */
               }

               if (skip > 0) { skip--; continue; }  /* 视口上方：不推进 y */
               if (n < maxrows && y + TREE_ROW_H <= bottom)
               {
                  rows[n].kind = RK_VAR; rows[n].slave = i; rows[n].var = v;
                  rows[n].rc.left = rc->left + 4; rows[n].rc.right = rc->right - 4;
                  rows[n].rc.top = y; rows[n].rc.bottom = y + TREE_ROW_H;
                  if (dir == 0)
                  {
                     rows[n].val.right = rc->right - 8;
                     rows[n].val.left = rows[n].val.right - 64;
                     rows[n].val.top = y + 2;
                     rows[n].val.bottom = y + TREE_ROW_H - 2;
                  }
                  else
                     rows[n].val.left = rows[n].val.right = 0;
                  n++;
               }
               y += TREE_ROW_H;
            }
         }
      }
   }
   return n;
}

/* 树总行数（用于滚动范围钳制） */
static int tree_count_rows(void)
{
   int n = 0, i, dir, v;
   for (i = 1; i <= g_snap.slavecount; ++i)
   {
      n++;
      if (!g_slave_exp[i]) continue;
      for (dir = 0; dir <= 1; ++dir)
      {
         int last_idx = -1;
         n++;
         if (!g_sec_exp[i][dir]) continue;
         for (v = 0; v < g_snap.vl.count; ++v)
         {
            const tc3_variable_t *var = &g_snap.vl.var[v];
            if (var->dir != dir) continue;
            if (var->slave != 0 && var->slave != i) continue;
            if ((int)var->index != last_idx)
            {
               last_idx = var->index;
               n++;   /* 地址分组行 */
            }
            n++;
         }
      }
   }
   return n;
}

/* ---------------- 变量树滚动条 ---------------- */

#define TREE_SB_W 10        /* 滚动条宽度 */

/* 可见行数（与 tree_build 底部裁剪一致） */
static int tree_vis_rows(const RECT *rc)
{
   /* 可见区 = 标题下 26px 到底部提示上沿 44px */
   int v = (rc->bottom - rc->top - 70) / TREE_ROW_H;
   return v < 1 ? 1 : v;
}

static void tree_clamp_scroll(const RECT *rc)
{
   int vis = tree_vis_rows(rc);
   int maxs = tree_count_rows() - vis;
   if (maxs < 0) maxs = 0;
   if (g_tree_scroll > maxs) g_tree_scroll = maxs;
   if (g_tree_scroll < 0) g_tree_scroll = 0;
}

/* 轨道矩形；返回 1 = 需要显示滚动条 */
static int tree_sb_track(const RECT *rc, RECT *trk)
{
   int total = tree_count_rows();
   int vis = tree_vis_rows(rc);

   trk->left = rc->right - 4 - TREE_SB_W;
   trk->right = rc->right - 4;
   trk->top = rc->top + 26;
   trk->bottom = rc->bottom - 44;   /* 底部留出两行操作提示 */
   if (trk->bottom - trk->top < 30) trk->bottom = trk->top + 30;
   return total > vis;
}

/* 滑块矩形（need 返回是否需要滚动条） */
static RECT tree_sb_thumb(const RECT *rc, int *need)
{
   RECT trk, th = { 0, 0, 0, 0 };
   int total = tree_count_rows(), vis = tree_vis_rows(rc), maxs, h;

   *need = tree_sb_track(rc, &trk);
   if (!*need) return th;

   maxs = total - vis;
   if (maxs < 1) maxs = 1;
   h = (int)((long long)(trk.bottom - trk.top) * vis / total);
   if (h < 18) h = 18;
   if (h > trk.bottom - trk.top) h = trk.bottom - trk.top;

   th.left = trk.left; th.right = trk.right;
   th.top = trk.top + (int)((long long)(trk.bottom - trk.top - h) *
                            g_tree_scroll / maxs);
   th.bottom = th.top + h;
   return th;
}

/* 内容区（右侧扣除滚动条宽度，绘制与命中共用） */
static RECT tree_content_rect(const RECT *rc)
{
   RECT trc = *rc, trk;
   if (tree_sb_track(rc, &trk)) trc.right -= (TREE_SB_W + 6);
   return trc;
}

/* 悬停浮框：由 draw_tree 检测并填充，WM_PAINT 在所有面板画完后调用
   tree_tooltip_paint 统一绘制（保证浮框压在波形区等其他面板之上） */
static int g_tip_show = 0;
static char g_tip_txt[192];
static int g_tip_x = 0, g_tip_y = 0;

static void tree_tooltip_paint(HDC hdc)
{
   wchar_t tw[192];
   SIZE tsz;
   int bw, bh, bx, by;
   RECT wr, box, tr2;
   HFONT oft;

   if (!g_tip_show)
      return;

   oft = (HFONT)SelectObject(hdc, g_font);
   u2w(g_tip_txt, tw, 192);
   GetTextExtentPoint32W(hdc, tw, (int)wcslen(tw), &tsz);
   SelectObject(hdc, oft);

   bw = tsz.cx + 16;
   bh = 22;
   bx = g_tip_x + 14;
   by = g_tip_y + 16;
   GetClientRect(g_hwnd, &wr);
   if (bx + bw > wr.right - 2) bx = g_tip_x - bw - 8;   /* 右缘翻左 */
   if (bx < 2) bx = 2;
   if (by + bh > wr.bottom - 2) by = g_tip_y - bh - 6;  /* 下缘翻上 */
   if (by < 2) by = 2;

   box.left = bx; box.top = by;
   box.right = bx + bw; box.bottom = by + bh;
   fill_rect_c(hdc, box, g_th->panel);
   frame_rect_c(hdc, box, g_th->accent);
   tr2.left = bx + 8; tr2.top = by;
   tr2.right = bx + bw - 6; tr2.bottom = by + bh;
   text_out(hdc, tr2, g_tip_txt, g_th->text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, g_font);
}

/* ---------------- CiA402 驱动控制区（绘制辅助） ---------------- */

/* 驱动状态对应的显示颜色 */
static COLORREF c402_state_color(int st)
{
   if (st == C402_ST_OPEN) return g_th->ok;
   if (st == C402_ST_FAULT || st == C402_ST_FREA) return g_th->err;
   if (st == C402_ST_NONE) return g_th->text_dim;
   return g_th->warn;
}

/* 驱动状态机状态名 */
static const char *c402_state_name(int st)
{
   switch (st)
   {
      case C402_ST_NOTREADY: return TR("未就绪", "Not ready");
      case C402_ST_SODIS:    return TR("未使能", "Switch-on disabled");
      case C402_ST_RTSO:     return TR("上电就绪", "Ready to switch on");
      case C402_ST_SON:      return TR("已开通", "Switched on");
      case C402_ST_OPEN:     return TR("运行使能", "Operation enabled");
      case C402_ST_FAULT:    return TR("故障", "Fault");
      case C402_ST_FREA:     return TR("故障响应", "Fault reaction");
      case C402_ST_QSTOP:    return TR("快速停止中", "Quick stop active");
      default:               return TR("未知", "Unknown");
   }
}

/* 运行模式码 -> 名称 */
static const char *c402_mode_name(int mo)
{
   switch (mo)
   {
      case 1:  return TR("位置 (PP)", "Position (PP)");
      case 2:  return TR("速度 (VL)", "Velocity (VL)");
      case 3:  return TR("回零 (HM)", "Homing (HM)");
      case 4:  return TR("力矩 (TQ)", "Torque (TQ)");
      case 7:  return TR("插补位置 (IP)", "Interpolated pos (IP)");
      case 8:  return TR("同步位置 (CSP)", "Cyclic sync pos (CSP)");
      case 9:  return TR("同步速度 (CSV)", "Cyclic sync vel (CSV)");
      case 10: return TR("同步力矩 (CST)", "Cyclic sync trq (CST)");
      default: return TR("其它", "Other");
   }
}

/* 按钮文本（左侧面板 2 列 × 4 行，按动作序号 0..6） */
static const char *c402_btn_label(int idx)
{
   switch (idx)
   {
      case 0: return TR("一键使能", "Auto En");
      case 1: return TR("故障复位", "Reset");
      case 2: return TR("快速停止", "Q-Stop");
      case 3: return TR("上电就绪", "Shutdown");
      case 4: return TR("开通", "Sw On");
      case 5: return TR("关使能", "Dis Op");
      case 6: return TR("禁止电压", "Dis Volt");
      default: return "";
   }
}

/* 每个按钮对应的 CiA402 动作码 */
static int c402_btn_action(int idx)
{
   switch (idx)
   {
      case 0: return C402_ACT_AUTOENABLE;
      case 1: return C402_ACT_RESET;
      case 2: return C402_ACT_QSTOP;
      case 3: return C402_ACT_SHUTDOWN;
      case 4: return C402_ACT_SWITCHON;
      case 5: return C402_ACT_DISABLE;
      case 6: return C402_ACT_DISVOLT;
      default: return 0;
   }
}

/* CiA402 小号按钮 */
static void c402_draw_btn(HDC hdc, RECT rc, const char *label, int hover, int enabled)
{
   wchar_t wl[64];
   COLORREF bg = enabled ? (hover ? g_th->hover : g_th->btnface) : g_th->head;

   u2w(label, wl, 64);
   fill_rect_c(hdc, rc, bg);
   frame_rect_c(hdc, rc, enabled ? g_th->border : g_th->head);
   {
      HFONT of = (HFONT)SelectObject(hdc, g_font_small);
      SetTextColor(hdc, enabled ? g_th->text : g_th->text_dim);
      SetBkMode(hdc, TRANSPARENT);
      DrawTextW(hdc, wl, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(hdc, of);
   }
}

static void draw_tree(HDC hdc, const RECT *rc, POINT mp)
{
   trow_t rows[512];
   int n, k;
   RECT trk, trc;
   int need_sb;

   draw_panel(hdc, *rc, TR("从站与变量", "Slaves && Variables"));

   /* 定时器每帧会用未连接快照覆盖，测试宏需每帧重新断言 */
#ifdef UI_TEST_TREE   /* 无硬件验证滚动条外观：cl -DUI_TEST_TREE */
   {
      int i, v;
      static int log_seeded = 0;
      g_snap.slavecount = 3;
      for (i = 1; i <= 3; ++i)
      {
         strcpy(g_snap.slave[i].name, "TestSlave");
         g_slave_exp[i] = 1;
         g_sec_exp[i][0] = 1;
         g_sec_exp[i][1] = 1;
      }
      g_snap.vl.count = 60;
      for (v = 0; v < 60; ++v)
      {
         strcpy(g_snap.vl.var[v].name, "TestVar");
         g_snap.vl.var[v].index = 0x6000 + v / 4;
         g_snap.vl.var[v].dir = v % 2;
         g_snap.vl.var[v].slave = (uint8)(v % 3) + 1;
      }
      /* 模拟 CiA402 驱动数据（无硬件验证布局） */
      g_snap.slave[1].c402 = 1;
      g_snap.slave[1].c402_sw = 0x1237;
      g_snap.slave[1].c402_state = C402_ST_OPEN;
      g_snap.slave[1].c402_mode = 8;
      g_snap.slave[2].c402 = 1;
      g_snap.slave[2].c402_sw = 0x000F;
      g_snap.slave[2].c402_state = C402_ST_FAULT;
      g_snap.slave[2].c402_mode = 9;
   }
#endif

   if (g_snap.slavecount == 0)
   {
      RECT tr = *rc; tr.top += 30; tr.left += 10;
      text_out(hdc, tr, g_snap.mstate == MST_IDLE ?
               TR("未连接总线\n\n点击左侧「扫描新的从站」\n选择网卡建立连接",
                  "Bus not connected\n\nClick \"Scan New Slaves\" on the left\nand pick a NIC to connect") :
               TR("正在扫描总线...", "Scanning the bus..."),
               g_th->text_dim, DT_LEFT, NULL);
      /* 未连接也显示操作提示，方便发现"点击行→输入→回车发送"（两行防裁剪） */
      tr = *rc; tr.left += 8; tr.bottom -= 4; tr.top = tr.bottom - 36;
      /* 横线分隔变量区与提示区 */
      {
         RECT sep = *rc; sep.left += 8; sep.right -= 8;
         sep.bottom = tr.top - 4; sep.top = sep.bottom - 1;
         fill_rect_c(hdc, sep, g_th->border);
      }
      text_out(hdc, tr, TR("输出：右键添加波形，双击数值输入\n输入：右键添加波形",
                           "Output: right-click to plot, double-click value\nInput: right-click to plot"),
               g_th->text_dim, DT_LEFT, g_font_small);
      return;
   }

   tree_clamp_scroll(rc);
   need_sb = tree_sb_track(rc, &trk);
   trc = tree_content_rect(rc);
   n = tree_build(rows, 512, &trc);

#ifdef UI_TIP_DEBUG   /* 临时调试：命中信息写窗口标题 */
   {
      char dbg[256];
      int hit = -1;
      for (int q = 0; q < n; ++q) if (pt_in(rows[q].rc, mp)) { hit = q; break; }
      snprintf(dbg, sizeof(dbg),
               "TIPDBG mp=(%d,%d) panel=(%d,%d-%d,%d) rows=%d hit=%d r0=(%d,%d-%d,%d)",
               mp.x, mp.y, rc->left, rc->top, rc->right, rc->bottom,
               n, hit, rows[0].rc.left, rows[0].rc.top,
               rows[0].rc.right, rows[0].rc.bottom);
      SetWindowTextA(g_hwnd, dbg);
   }
#endif

   /* 悬停浮框状态：名字被省略号截断的行，鼠标悬停时在旁边显示完整名字。
      这里只记录，由 WM_PAINT 末尾的 tree_tooltip_paint 统一绘制 */
   g_tip_show = 0;
   g_tip_txt[0] = '\0';
   g_tip_x = mp.x; g_tip_y = mp.y;

   for (k = 0; k < n; ++k)
   {
      const trow_t *r = &rows[k];
      char line[192];
      wchar_t wl[192];

      if (r->kind == RK_SLAVE)
      {
         const ui_slave_info_t *s = &g_snap.slave[r->slave];
         HBRUSH b;
         RECT dot;

         snprintf(line, sizeof(line), "%s Slave%d  %s",
                  g_slave_exp[r->slave] ? "▾" : "▸", r->slave, s->name);
         u2w(line, wl, 192);
         if (pt_in(r->rc, mp))
         {
            fill_rect_c(hdc, r->rc, g_th->hover);
            /* 从站行名字被截断时记录悬停浮框（完整行文本） */
            {
               RECT tr0 = r->rc; tr0.left += 6; tr0.right -= 24;
               SIZE tsz;
               HFONT oft = (HFONT)SelectObject(hdc, g_font);
               GetTextExtentPoint32W(hdc, wl, (int)wcslen(wl), &tsz);
               SelectObject(hdc, oft);
               if (tsz.cx > tr0.right - tr0.left)
               {
                  g_tip_show = 1;
                  snprintf(g_tip_txt, sizeof(g_tip_txt), "Slave%d  %s", r->slave, s->name);
                  g_tip_x = mp.x; g_tip_y = mp.y;
               }
            }
         }
         SetBkMode(hdc, TRANSPARENT);
         SetTextColor(hdc, g_th->accent);   /* 从站行：蓝色 */
         {
            RECT tr = r->rc; tr.left += 6;
            HFONT oft = (HFONT)SelectObject(hdc, g_font);   /* 显式常规字体，防粗体残留 */
            DrawTextW(hdc, wl, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, oft);
         }
         dot.left = r->rc.right - 14; dot.top = r->rc.top + 6;
         dot.right = dot.left + 10; dot.bottom = dot.top + 10;
         b = CreateSolidBrush(state_color(s));
         FillRect(hdc, &dot, b);
         DeleteObject(b);
      }
      else if (r->kind == RK_SECTION)
      {
         snprintf(line, sizeof(line), "    %s %s",
                  g_sec_exp[r->slave][r->var] ? "▾" : "▸",
                  r->var == 0 ? "Output" : "Input");
         if (pt_in(r->rc, mp)) fill_rect_c(hdc, r->rc, g_th->hover);
         text_out(hdc, r->rc, line, g_th->err, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                  g_font_bold);
      }
      else if (r->kind == RK_ADDR)
      {
         /* CoE 对象地址分组行：绿色 #0xXXXX（如 #0x6010），不带"变量地址"字样 */
         snprintf(line, sizeof(line), "        #0x%04X",
                  g_snap.vl.var[r->var].index);
         if (pt_in(r->rc, mp)) fill_rect_c(hdc, r->rc, g_th->hover);
         text_out(hdc, r->rc, line, g_th->ok, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                  g_font_bold);
      }
      else
      {
         const tc3_variable_t *v = &g_snap.vl.var[r->var];
         int ch = -1, ci;
         wchar_t wn[80];

         for (ci = 0; ci < g_chan_count; ++ci)
            if (g_chan[ci].var == r->var) { ch = ci; break; }

         if (ch >= 0)   /* 正在显示波形：着色 */
         {
            fill_rect_c(hdc, r->rc, g_th->sel);
            {
               HBRUSH b = CreateSolidBrush(CHAN_COLORS[ch % CHAN_MAX]);
               RECT dot = r->rc; dot.left += 58; dot.top += 5;
               dot.right = dot.left + 9; dot.bottom = dot.top + 9;
               FillRect(hdc, &dot, b);
               DeleteObject(b);
            }
         }
         else if (pt_in(r->rc, mp))
            fill_rect_c(hdc, r->rc, g_th->hover);

         u2w(v->name, wn, 80);
         SetBkMode(hdc, TRANSPARENT);
         {
            /* 变量名整体右移，使其位于地址行（#0xXXXX）的右下方，层级更清晰
               显式选常规字体：地址分组行是粗体，残留 DC 状态会让变量名误变粗体 */
            RECT tr = r->rc; tr.left += (ch >= 0) ? 74 : 64; tr.right -= 70;
            {
               HFONT oft = (HFONT)SelectObject(hdc, g_font);
               SetTextColor(hdc, g_th->text);
               DrawTextW(hdc, wn, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
               SelectObject(hdc, oft);
            }

            /* 悬停且名字被省略号截断：记录浮框，循环结束后统一画在最上层 */
            if (pt_in(r->rc, mp))
            {
               SIZE tsz;
               HFONT oft = (HFONT)SelectObject(hdc, g_font);
               GetTextExtentPoint32W(hdc, wn, (int)wcslen(wn), &tsz);
               SelectObject(hdc, oft);
               if (tsz.cx > tr.right - tr.left)
               {
                  g_tip_show = 1;
                  snprintf(g_tip_txt, sizeof(g_tip_txt), "%s", v->name);
                  g_tip_x = mp.x; g_tip_y = mp.y;
               }
            }
         }

         if (r->val.right > r->val.left)   /* 输出变量：数值自然显示（双击进入编辑） */
         {
            char vb[32];
            snprintf(vb, sizeof(vb), "%lld", g_snap.oval[r->var]);
            {
               RECT tr = r->val; tr.right -= 4;
               /* 悬停/编辑：只高亮数值本身（数字实际宽度内的底色），
                  不再画整宽下划线（数字靠右、左边空白会露出一条游离的横线） */
               if (g_hover_val_var == r->var || g_editing_idx == r->var)
               {
                  SIZE tsz;
                  RECT hl;
                  HFONT oft = (HFONT)SelectObject(hdc, g_font);
                  GetTextExtentPoint32A(hdc, vb, (int)strlen(vb), &tsz);
                  SelectObject(hdc, oft);
                  hl = tr;
                  hl.left = hl.right - tsz.cx - 6;
                  if (hl.left < tr.left) hl.left = tr.left;
                  fill_rect_c(hdc, hl, g_th->sel);
               }
               text_out(hdc, tr, vb, g_th->accent,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE, g_font);
            }
         }
         else   /* 输入变量：普通数值 */
         {
            char vb[32];
            snprintf(vb, sizeof(vb), "%lld", g_snap.ival[r->var]);
            {
               RECT tr = r->rc; tr.right -= 8;
               text_out(hdc, tr, vb, g_th->text_dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, NULL);
            }
         }
      }
   }

   /* 滚动条（轨道 + 滑块），让用户知道还有多少内容 */
   if (need_sb)
   {
      RECT th = tree_sb_thumb(rc, &need_sb);
      fill_rect_c(hdc, trk, g_th->valbox);
      frame_rect_c(hdc, trk, g_th->border);
      fill_rect_c(hdc, th, (g_sb_drag || pt_in(th, mp)) ? g_th->accent : g_th->text_dim);
   }

   /* 底部提示（最后画，避免被变量行覆盖；两行防右边缘裁剪） */
   {
      RECT tr = *rc; tr.left += 8; tr.bottom -= 4; tr.top = tr.bottom - 36;
      /* 横线分隔变量区与提示区 */
      {
         RECT sep = *rc; sep.left += 8; sep.right -= 8;
         sep.bottom = tr.top - 4; sep.top = sep.bottom - 1;
         fill_rect_c(hdc, sep, g_th->border);
      }
      text_out(hdc, tr, TR("输出：右键添加波形，双击数值输入\n输入：右键添加波形",
                           "Output: right-click to plot, double-click value\nInput: right-click to plot"),
               g_th->text_dim, DT_LEFT, g_font_small);
   }
}

/* ============================ 左侧 CiA402 驱动控制区 ============================ */

#define C402_HDR_H     22     /* 标题行高 */
#define C402_LINE_H    17     /* 状态/模式行高 */
#define C402_BTN_H     22     /* 控制按钮高 */
#define C402_BTN_GAP   4      /* 按钮行间距 */

/* 左侧 CiA402 区所有命中矩形（绘制与点击共用） */
typedef struct
{
   int  has;                  /* 1 = 总线上有映射了 CiA402 的从站 */
   int  slave;                /* 当前显示的从站号（0 = 无） */
   int  multi;                /* 1 = 有多个 CiA402 从站，点击可切换 */
   RECT sec;                  /* 整区（背景/边界） */
   RECT hdr;                  /* 标题行（点击折叠/展开） */
   RECT sel;                  /* 驱动选择行（点击切换从站） */
   RECT state;                /* 驱动状态行（点击弹协议说明） */
   RECT mode;                 /* 运行模式行（点击选择模式） */
   RECT btn[7];               /* 控制按钮 */
   int  nbtn;
} c402_ui_t;

/* 当前应显示的从站：优先用户选中的，否则取第一个带 CiA402 的从站 */
static int c402_cur_slave(void)
{
   int i, first = 0;
   if (g_c402_sel >= 1 && g_c402_sel <= g_snap.slavecount &&
       g_snap.slave[g_c402_sel].c402)
      return g_c402_sel;
   for (i = 1; i <= g_snap.slavecount; ++i)
      if (g_snap.slave[i].c402) { first = i; break; }
   return first;
}

/* 计算 CiA402 区各矩形；rc = 左侧面板矩形 */
static void c402_left_layout(const RECT *rc, c402_ui_t *u)
{
   RECT b_scan, b_flash, b_mode, b_pause;
   int x = rc->left + 10, w = (rc->right - rc->left) - 20;
   int y, r, c, n = 0, bw, i;

   memset(u, 0, sizeof(*u));
   left_buttons(rc, &b_scan, &b_flash, &b_mode, &b_pause);

   u->slave = c402_cur_slave();
   u->has = (u->slave > 0);
   for (i = 1; i <= g_snap.slavecount; ++i)
      if (g_snap.slave[i].c402 && i != u->slave) { u->multi = 1; break; }

   y = b_pause.bottom + 12;
   u->sec.left = rc->left + 6; u->sec.right = rc->right - 6;
   u->sec.top = y;

   u->hdr.left = x; u->hdr.right = x + w;
   u->hdr.top = y; u->hdr.bottom = y + C402_HDR_H;
   y = u->hdr.bottom;

   if (!g_c402_open)
   {
      u->sec.bottom = y + 2;
      return;
   }

   u->sel.left = x; u->sel.right = x + w;
   u->sel.top = y; u->sel.bottom = y + C402_LINE_H;
   y = u->sel.bottom + 1;

   u->state = u->sel;
   u->state.top = y; u->state.bottom = y + C402_LINE_H;
   y = u->state.bottom + 1;

   if (!u->has)   /* 没有驱动器：只留一行提示 */
   {
      u->mode.left = u->mode.right = 0;
      u->sec.bottom = u->state.bottom + 4;
      return;
   }

   u->mode = u->sel;
   u->mode.top = y; u->mode.bottom = y + C402_LINE_H;
   y = u->mode.bottom + 4;

   /* 2 列 × 4 行：一键使能/故障复位、快速停止/上电就绪、开通/关使能、禁止电压 */
   bw = (w - 4) / 2;
   for (r = 0; r < 4 && n < 7; ++r)
   {
      for (c = 0; c < 2 && n < 7; ++c)
      {
         u->btn[n].left = x + c * (bw + 4);
         u->btn[n].right = u->btn[n].left + bw;
         u->btn[n].top = y;
         u->btn[n].bottom = y + C402_BTN_H;
         if (r == 3 && c == 0) u->btn[n].right = x + w;   /* 末行单个按钮拉通 */
         n++;
      }
      y += C402_BTN_H + C402_BTN_GAP;
   }
   u->nbtn = n;
   u->sec.bottom = y - C402_BTN_GAP + 4;
}

/* 绘制 CiA402 区 */
static void draw_c402_left(HDC hdc, const c402_ui_t *u, POINT mp)
{
   const ui_slave_info_t *s = u->has ? &g_snap.slave[u->slave] : NULL;
   char line[160];
   int i, can;

   /* 标题行：折叠箭头 + 名称（点击折叠/展开） */
   if (pt_in(u->hdr, mp)) fill_rect_c(hdc, u->hdr, g_th->hover);
   snprintf(line, sizeof(line), "%s %s", g_c402_open ? "▾" : "▸",
            TR("CiA402 驱动控制", "CiA402 Drive Control"));
   text_out(hdc, u->hdr, line, g_th->accent,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_bold);
   if (!g_c402_open) return;

   /* 驱动选择行 */
   if (pt_in(u->sel, mp) && u->multi) fill_rect_c(hdc, u->sel, g_th->hover);
   if (u->has)
      snprintf(line, sizeof(line), TR("驱动 #%d %s%s", "Drive #%d %s%s"),
               u->slave, s->name, u->multi ? " ▸" : "");
   else
      snprintf(line, sizeof(line), "%s", TR("未检测到驱动器", "No drive detected"));
   text_out(hdc, u->sel, line, u->has ? g_th->text : g_th->text_dim,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_small);
   if (!u->has) return;

   /* 状态行：颜色圆点 + 状态名 + 状态字原始值（点击弹协议说明） */
   if (pt_in(u->state, mp)) fill_rect_c(hdc, u->state, g_th->hover);
   {
      RECT dot, tr = u->state;
      HBRUSH b;

      dot.left = u->state.left + 1; dot.top = u->state.top + 4;
      dot.right = dot.left + 9;     dot.bottom = dot.top + 9;
      b = CreateSolidBrush(c402_state_color(s->c402_state));
      FillRect(hdc, &dot, b);
      DeleteObject(b);
      frame_rect_c(hdc, dot, g_th->border);

      tr.left = dot.right + 5;
      snprintf(line, sizeof(line), "%s  0x%04X",
               c402_state_name(s->c402_state), s->c402_sw);
      text_out(hdc, tr, line, c402_state_color(s->c402_state),
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_small);
   }

   /* 运行模式行（点击弹出模式列表） */
   if (pt_in(u->mode, mp)) fill_rect_c(hdc, u->mode, g_th->hover);
   if (s->c402_mode >= 0)
      snprintf(line, sizeof(line), TR("模式: %s", "Mode: %s"),
               c402_mode_name(s->c402_mode));
   else
      snprintf(line, sizeof(line), "%s", TR("模式: 无 0x6061", "Mode: none"));
   text_out(hdc, u->mode, line, (s->c402_mode >= 0) ? g_th->text : g_th->text_dim,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_small);

   /* 控制按钮（未连接总线时整体置灰） */
   can = (g_snap.mstate == MST_RUNNING);
   for (i = 0; i < u->nbtn; ++i)
      c402_draw_btn(hdc, u->btn[i], c402_btn_label(i),
                    can && pt_in(u->btn[i], mp), can);
}

/* ============================ 左侧操作面板 ============================ */

static void draw_left(HDC hdc, const RECT *rc, POINT mp)
{
   RECT b_scan, b_flash, b_mode, b_pause, tr;
   c402_ui_t cu;
   int y, i;

#ifdef UI_TEST_TREE   /* 无硬件验证：每帧断言 CiA402 模拟数据（2 台驱动） */
   g_snap.slavecount = 3;
   strcpy(g_snap.slave[1].name, "TestServo");
   g_snap.slave[1].c402 = 1; g_snap.slave[1].c402_sw = 0x1237;
   g_snap.slave[1].c402_state = C402_ST_OPEN; g_snap.slave[1].c402_mode = 8;
   strcpy(g_snap.slave[2].name, "TestServo2");
   g_snap.slave[2].c402 = 1; g_snap.slave[2].c402_sw = 0x000F;
   g_snap.slave[2].c402_state = C402_ST_FAULT; g_snap.slave[2].c402_mode = 9;
#endif

   draw_panel(hdc, *rc, TR("操作", "Actions"));
   left_buttons(rc, &b_scan, &b_flash, &b_mode, &b_pause);
   draw_button(hdc, b_scan, TR("扫描新的从站", "Scan New Slaves"), pt_in(b_scan, mp), 0, 1, 0);
   draw_button(hdc, b_flash, TR("烧录XML文件", "Flash XML File"), pt_in(b_flash, mp), 0,
               g_snap.mstate != MST_IDLE, 0);
   draw_button(hdc, b_mode,
               g_snap.dc_active ? TR("从站模式: DC", "Mode: DC")
                                : TR("从站模式: FreeRUN", "Mode: FreeRUN"),
               pt_in(b_mode, mp), 0, g_snap.mstate != MST_IDLE, 0);
   draw_button(hdc, b_pause,
               g_paused ? TR("继续刷新", "Resume") : TR("暂停刷新", "Pause"),
               pt_in(b_pause, mp), 0, 1, g_paused);

   /* CiA402 驱动控制区（紧凑版，位于操作按钮下方） */
   c402_left_layout(rc, &cu);
   draw_c402_left(hdc, &cu, mp);

   /* 从站连接状态 */
   tr.left = rc->left + 10; tr.right = rc->right - 8;
   tr.top = cu.sec.bottom + 10; tr.bottom = tr.top + 18;
   text_out(hdc, tr, TR("从站连接状态", "Slave Status"),
            g_th->text_dim, DT_LEFT | DT_SINGLELINE, g_font_bold);
   y = tr.bottom + 4;

   for (i = 1; i <= g_snap.slavecount; ++i)
   {
      const ui_slave_info_t *s = &g_snap.slave[i];
      char line[128];
      HBRUSH b;
      RECT dot;

      /* 行 1：圆点 + #i 名称 */
      snprintf(line, sizeof(line), "#%d %s", i, s->name);
      tr.top = y; tr.bottom = y + 17;
      tr.left = rc->left + 26; tr.right = rc->right - 8;
      text_out(hdc, tr, line, s->islost ? g_th->err : g_th->text,
               DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, NULL);
      dot.left = rc->left + 10; dot.top = y + 4;
      dot.right = dot.left + 10; dot.bottom = dot.top + 10;
      b = CreateSolidBrush(state_color(s));
      FillRect(hdc, &dot, b);
      DeleteObject(b);
      y += 17;

      /* 行 2：状态标签 + AL 错误 */
      snprintf(line, sizeof(line), "    %s%s", s->label,
               s->alstatus ? "" : "");
      tr.top = y; tr.bottom = y + 16;
      text_out(hdc, tr, line, g_th->text_dim, DT_LEFT | DT_SINGLELINE, g_font_small);
      y += 18;
      if (y > rc->bottom - (g_snap.flash_phase == 1 ? 56 : 8)) break;
   }
   if (g_snap.slavecount == 0)
   {
      tr.top = y + 2; tr.bottom = y + 20;
      text_out(hdc, tr, g_snap.mstate == MST_IDLE ? TR("（未连接）", "(not connected)")
                                                  : TR("（扫描中...）", "(scanning...)"),
               g_th->text_dim, DT_LEFT | DT_SINGLELINE, g_font_small);
   }

   /* 烧录进度：固定在操作面板最下面（flash_phase==1 = 构建镜像/烧录中） */
   {
      int fl_phase = g_snap.flash_phase;
      int fl_done = g_snap.flash_done;
      int fl_total = g_snap.flash_total;
      int fl_slave = g_snap.flash_slave;
      const char *fl_file = g_snap.flash_file;
#ifdef UI_TEST_FLASH   /* 无硬件时验证进度条外观：cl -DUI_TEST_FLASH */
      fl_phase = 1; fl_done = 7; fl_total = 10; fl_file = "test_device.xml";
      fl_slave = 2;
#endif
      if (fl_phase == 1)
      {
         RECT bar, fillb;
         int pct = 0;
         char line[192];

         if (fl_total > 0)
            pct = (int)((long long)fl_done * 100 / fl_total);
         if (pct < 0) pct = 0;
         if (pct > 100) pct = 100;

         snprintf(line, sizeof(line), TR("从站%d 烧录 %d%%  %s",
                                         "Slave%d flash %d%%  %s"), fl_slave, pct, fl_file);
         tr.left = rc->left + 10; tr.right = rc->right - 8;
         tr.top = rc->bottom - 46; tr.bottom = tr.top + 16;
         text_out(hdc, tr, line, g_th->text,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_small);

         bar.left = rc->left + 10; bar.right = rc->right - 8;
         bar.top = rc->bottom - 26; bar.bottom = bar.top + 12;
         fill_rect_c(hdc, bar, g_th->valbox);
         frame_rect_c(hdc, bar, g_th->border);
         if (pct > 0)
         {
            fillb = bar;
            fillb.right = bar.left + (int)((long long)(bar.right - bar.left) * pct / 100);
            if (fillb.right <= fillb.left) fillb.right = fillb.left + 1;
            fill_rect_c(hdc, fillb, g_th->ok);   /* 绿色进度条（与主题配色的 OP 状态绿一致） */
         }
      }
   }
}

/* ============================ 消息日志区 ============================ */

/* 消息文字区的可视矩形（表头下方，用于拖选命中判断与 I 型光标） */
static RECT log_text_rect(const RECT *rc)
{
   RECT r;
   r.left = rc->left + 6;
   r.right = rc->right - 6;
   r.top = rc->top + 24;
   r.bottom = rc->bottom - 4;
   return r;
}

/* 当前视窗布局：start = 首个可见行的绝对索引，vis = 可见行数 */
static void log_layout(const RECT *rc, int *start, int *vis)
{
   int v = (rc->bottom - rc->top - 26) / LOG_LINE_H;
   if (v < 1) v = 1;
   if (g_log_scroll > g_log_count - v) g_log_scroll = g_log_count - v;
   if (g_log_scroll < 0) g_log_scroll = 0;
   *vis = v;
   *start = g_log_count - v - g_log_scroll;
}

/* 鼠标 y 坐标 -> 消息绝对行索引（越界钳制到首/末可见行；无消息返回 -1） */
static int log_row_at(const RECT *rc, POINT p)
{
   int start, vis, row, idx;

   if (g_log_count <= 0) return -1;
   log_layout(rc, &start, &vis);
   row = (p.y - (rc->top + 26)) / LOG_LINE_H;
   if (row < 0) row = 0;
   if (row > vis - 1) row = vis - 1;
   idx = start + row;
   if (idx < 0) idx = 0;
   if (idx > g_log_count - 1) idx = g_log_count - 1;
   return idx;
}

/* 取某行显示文本的宽字符形式（"[时间]  正文"），返回字符数（不含结尾 0） */
static int log_row_w(int idx, wchar_t *w, int wcap)
{
   char line[256];
   int n;

   if (idx < 0 || idx >= g_log_count) return 0;
   {
      const logline_t *l = log_get(idx);
      snprintf(line, sizeof(line), "[%s]  %s", l->t, l->text);
   }
   n = MultiByteToWideChar(CP_UTF8, 0, line, -1, w, wcap);
   return (n > 0) ? n - 1 : 0;
}

/* 宽字符串前 ci 个字符的像素宽度（hdc 需已选好消息字体） */
static int log_w_prefix(HDC hdc, const wchar_t *w, int ci, int wlen)
{
   SIZE sz;

   if (ci <= 0 || wlen <= 0) return 0;
   if (ci > wlen) ci = wlen;
   if (!GetTextExtentPoint32W(hdc, w, ci, &sz)) return 0;
   return sz.cx;
}

/* 像素 x 偏移 -> 字符列号（0..wlen）：落在字符左半侧取该字符，右半侧取下一个 */
static int log_w_col(HDC hdc, const wchar_t *w, int wlen, int rx)
{
   int i, prev = 0;
   SIZE sz;

   if (rx <= 0 || wlen <= 0) return 0;
   for (i = 1; i <= wlen; ++i)
   {
      if (!GetTextExtentPoint32W(hdc, w, i, &sz)) return i - 1;
      if (rx < (prev + sz.cx) / 2) return i - 1;
      prev = sz.cx;
   }
   return wlen;
}

/* 鼠标 x 坐标 -> 某行的字符列号（用窗口 DC + 消息字体测量） */
static int log_col_at_x(const RECT *rc, int row, int px)
{
   wchar_t w[300];
   int wlen, col;
   HDC hdc;
   HFONT of;

   wlen = log_row_w(row, w, 300);
   if (wlen <= 0) return 0;
   hdc = GetDC(g_hwnd);
   if (hdc == NULL) return 0;
   of = (HFONT)SelectObject(hdc, g_font_small);
   col = log_w_col(hdc, w, wlen, px - (rc->left + 10));
   SelectObject(hdc, of);
   ReleaseDC(g_hwnd, hdc);
   return col;
}

/* 选择区间排序：(r0,c0) 在前，(r1,c1) 在后；无选择时 r0 = -1 */
static void log_sel_order(int *r0, int *c0, int *r1, int *c1)
{
   int a = g_log_sel_a, ac = g_log_sel_ac;
   int b = g_log_sel_b, bc = g_log_sel_bc;

   if (a < 0 || b < 0) { *r0 = -1; *c0 = 0; *r1 = -1; *c1 = 0; return; }
   if (a > b || (a == b && ac > bc))
   {
      *r0 = b; *c0 = bc; *r1 = a; *c1 = ac;
   }
   else
   {
      *r0 = a; *c0 = ac; *r1 = b; *c1 = bc;
   }
   if (*r0 > g_log_count - 1) *r0 = g_log_count - 1;
   if (*r1 > g_log_count - 1) *r1 = g_log_count - 1;
   if (*r0 < 0) *r0 = 0;
}

/* UTF-8 文本写入剪贴板（CF_UNICODETEXT） */
static void clipboard_set_utf8(const char *utf8)
{
   int wlen;
   HGLOBAL h;
   wchar_t *w;

   if (utf8 == NULL || utf8[0] == '\0') return;
   wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
   if (wlen <= 0) return;
   h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(wchar_t));
   if (h == NULL) return;
   w = (wchar_t *)GlobalLock(h);
   if (w == NULL) { GlobalFree(h); return; }
   MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen);
   GlobalUnlock(h);

   if (!OpenClipboard(g_hwnd)) { GlobalFree(h); return; }
   EmptyClipboard();
   if (SetClipboardData(CF_UNICODETEXT, h) == NULL)
      GlobalFree(h);            /* 失败时所有权未转移，需自行释放 */
   CloseClipboard();
}

/* 把选中的片段（按字符）复制到剪贴板，返回有内容的行数（0 = 无选择） */
static int log_copy_sel(void)
{
   int r0, c0, r1, c1, r, n = 0;
   size_t cap, len = 0;
   char *buf;

   if (g_log_count <= 0) return 0;
   log_sel_order(&r0, &c0, &r1, &c1);
   if (r0 < 0 || r1 < r0) return 0;
   if (r0 == r1 && c0 == c1) return 0;      /* 空选择 */

   cap = (size_t)(r1 - r0 + 1) * 1400 + 8;
   buf = (char *)malloc(cap);
   if (buf == NULL) return 0;
   buf[0] = '\0';

   for (r = r0; r <= r1; ++r)
   {
      wchar_t w[300];
      char u8[1400];
      int wlen = log_row_w(r, w, 300);
      int s = (r == r0) ? c0 : 0;
      int e = (r == r1) ? c1 : wlen;
      int un;

      if (s < 0) s = 0;
      if (e > wlen) e = wlen;
      if (e > s)
      {
         un = WideCharToMultiByte(CP_UTF8, 0, w + s, e - s, u8, (int)sizeof(u8), NULL, NULL);
         if (un > 0 && len + (size_t)un + 3 <= cap)
         {
            memcpy(buf + len, u8, (size_t)un);
            len += (size_t)un;
            n++;
         }
      }
      /* 跨行片段：行间补 CRLF（末行不补） */
      if (r < r1 && len + 3 <= cap)
      {
         buf[len++] = '\r';
         buf[len++] = '\n';
      }
   }
   buf[len] = '\0';

   clipboard_set_utf8(buf);
   free(buf);
   return n;
}

static void draw_log(HDC hdc, const RECT *rc, POINT mp)
{
   RECT clr = log_clear_rect(rc);
   int vis, start, i, r0, c0, r1, c1;
   int text_left = rc->left + 10;

   draw_panel(hdc, *rc, TR("消息", "Messages"));
   draw_button(hdc, clr, TR("清空", "Clear"), pt_in(clr, mp), 0, 1, 0);

   /* 错误计数：表头"清空"按钮左侧显示 ERR: N（有错误时红色，无错误灰色） */
   {
      char eb[32];
      RECT er;
      snprintf(eb, sizeof(eb), "ERR: %d", g_err_count);
      er.left = clr.left - 96; er.right = clr.left - 10;
      er.top = rc->top + 3;    er.bottom = er.top + 18;
      text_out(hdc, er, eb, g_err_count > 0 ? g_th->err : g_th->text_dim,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE, g_font_bold);
   }

   log_layout(rc, &start, &vis);
   log_sel_order(&r0, &c0, &r1, &c1);

   /* 选中片段高亮：逐行按字符边界量出 x 范围再铺底色 */
   if (r0 >= 0 && g_log_count > 0)
   {
      HFONT ofs = (HFONT)SelectObject(hdc, g_font_small);

      for (i = 0; i < vis; ++i)
      {
         int idx = start + i, wlen, s, e;
         wchar_t w[300];
         RECT hb;

         if (idx < r0 || idx > r1) continue;
         if (idx < 0 || idx >= g_log_count) continue;
         wlen = log_row_w(idx, w, 300);
         s = (idx == r0) ? c0 : 0;
         e = (idx == r1) ? c1 : wlen;
         if (s < 0) s = 0;
         if (e > wlen) e = wlen;
         if (e <= s) continue;

         hb.left = text_left + log_w_prefix(hdc, w, s, wlen);
         hb.right = text_left + log_w_prefix(hdc, w, e, wlen);
         hb.top = rc->top + 26 + i * LOG_LINE_H;
         hb.bottom = hb.top + LOG_LINE_H;
         if (hb.right <= hb.left) hb.right = hb.left + 1;
         fill_rect_c(hdc, hb, g_th->sel);
      }
      SelectObject(hdc, ofs);
   }

   for (i = 0; i < vis; ++i)
   {
      int idx = start + i;
      const logline_t *l;
      char line[256];
      RECT tr;

      if (idx < 0 || idx >= g_log_count) continue;
      l = log_get(idx);
      snprintf(line, sizeof(line), "[%s]  %s", l->t, l->text);
      tr.left = rc->left + 10; tr.right = rc->right - 10;
      tr.top = rc->top + 26 + i * LOG_LINE_H;
      tr.bottom = tr.top + LOG_LINE_H;
      text_out(hdc, tr, line, l->err ? g_th->err : g_th->text,
               DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_small);
   }
}

/* ============================ 顶栏 ============================ */

/* 标题文本实际像素宽度（大号字体，动态测量避免固定盒子截断） */
static int topbar_title_width(void)
{
   static int cached = -1;
   if (cached < 0 && g_font_title != NULL)
   {
      HDC hdc = GetDC(g_hwnd);
      wchar_t wt[64];
      SIZE sz;
      HFONT of;
      u2w("SimpleCAT V1.0", wt, 64);
      of = (HFONT)SelectObject(hdc, g_font_title);
      GetTextExtentPoint32W(hdc, wt, (int)wcslen(wt), &sz);
      SelectObject(hdc, of);
      ReleaseDC(g_hwnd, hdc);
      cached = sz.cx;
   }
   return cached > 0 ? cached : 140;
}

/* 测量顶栏菜单文本宽度（g_font），供菜单矩形动态布局 */
static int topbar_menu_text_width(const char *txt)
{
   HDC hdc;
   wchar_t wt[64];
   SIZE sz;
   HFONT of;
   int w;

   if (g_font == NULL)
      return 56;
   hdc = GetDC(g_hwnd);
   u2w(txt, wt, 64);
   of = (HFONT)SelectObject(hdc, g_font);
   GetTextExtentPoint32W(hdc, wt, (int)wcslen(wt), &sz);
   SelectObject(hdc, of);
   ReleaseDC(g_hwnd, hdc);
   w = sz.cx;
   return w > 0 ? w : 56;
}

static void topbar_menu_rects(RECT *m1, RECT *m2, RECT *m3, RECT *m4)
{
   /* 菜单紧跟标题之后，不重叠；矩形宽度按当前语言的菜单文本实测，
      中文窄英文宽（Settings/About），避免相邻菜单文字重叠 */
   int x = 12 + topbar_title_width() + 24;
   int w1 = topbar_menu_text_width(TR("菜单", "Bus")) + 28;
   int w2 = topbar_menu_text_width(TR("设置", "Settings")) + 28;
   int w3 = topbar_menu_text_width(TR("皮肤", "Skin")) + 28;
   int w4 = topbar_menu_text_width(TR("软件信息", "About")) + 40;
   if (x < 170) x = 170;
   m1->left = x;        m1->top = 0; m1->right = x + w1;          m1->bottom = TOP_H;
   m2->left = x + w1;   m2->top = 0; m2->right = x + w1 + w2;     m2->bottom = TOP_H;
   m3->left = x + w1 + w2; m3->top = 0; m3->right = x + w1 + w2 + w3; m3->bottom = TOP_H;
   m4->left = x + w1 + w2 + w3; m4->top = 0;
   m4->right = x + w1 + w2 + w3 + w4;                              m4->bottom = TOP_H;
}

static void draw_topbar(HDC hdc, const RECT *rc, POINT mp)
{
   RECT m1, m2, m3, m4;
   RECT tr;
   const char *st;
   char stx[160];
   int i, n_op = 0;

   fill_rect_c(hdc, *rc, g_th->bg);

   /* 标题（矩形按文本实际宽度取，高度取整个顶栏 + DT_VCENTER 垂直居中，
      避免 DrawTextW 按矩形裁剪导致文字底部被削掉） */
   tr = *rc; tr.left = 12;
   tr.right = tr.left + topbar_title_width() + 10;
   text_out(hdc, tr, "SimpleCAT V1.0", g_th->accent,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, g_font_title);

   /* 菜单文本（悬停高亮） */
   topbar_menu_rects(&m1, &m2, &m3, &m4);
   if (pt_in(m1, mp)) fill_rect_c(hdc, m1, g_th->hover);
   if (pt_in(m2, mp)) fill_rect_c(hdc, m2, g_th->hover);
   if (pt_in(m3, mp)) fill_rect_c(hdc, m3, g_th->hover);
   if (pt_in(m4, mp)) fill_rect_c(hdc, m4, g_th->hover);
   tr = m1; tr.top += 6; tr.bottom -= 6;
   text_out(hdc, tr, TR("菜单", "Bus"), g_th->text, DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);
   tr = m2; tr.top += 6; tr.bottom -= 6;
   text_out(hdc, tr, TR("设置", "Settings"), g_th->text, DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);
   tr = m3; tr.top += 6; tr.bottom -= 6;
   text_out(hdc, tr, TR("皮肤", "Skin"), g_th->text, DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);
   tr = m4; tr.top += 6; tr.bottom -= 6;
   text_out(hdc, tr, TR("软件信息", "About"), g_th->text, DT_CENTER | DT_VCENTER | DT_SINGLELINE, NULL);

   /* 右侧状态文本 */
   switch (g_snap.mstate)
   {
      case MST_RUNNING:   st = TR("运行中", "Running");     break;
      case MST_STARTING:  st = TR("启动中", "Starting");    break;
      case MST_STARTFAIL: st = TR("启动失败", "Start fail"); break;
      default:            st = TR("未连接", "Disconnected"); break;
   }
   for (i = 1; i <= g_snap.slavecount; ++i)
      if (g_snap.slave[i].state == EC_STATE_OPERATIONAL) n_op++;
   if (g_snap.mstate == MST_RUNNING)
      snprintf(stx, sizeof(stx), "%s  WKC %d/%d  OP %d/%d  %s", st,
               g_snap.wkc, g_snap.expected, n_op, g_snap.slavecount,
               g_snap.dc_active ? "DC" : "FreeRUN");
   else
      snprintf(stx, sizeof(stx), "%s", st);
   tr = *rc; tr.right -= 12; tr.left = tr.right - 320;
   tr.top += 7; tr.bottom -= 7;
   text_out(hdc, tr, stx, g_th->text_dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, NULL);

   /* 状态指示灯：状态文字左侧的圆形（未连接=红，运行中=绿）
    * 指示灯左边实时显示当前同步周期（实测主循环周期，ms） */
   {
      wchar_t ws[256], wc[64];
      SIZE sz;
      int d = 12, gap = 7, txw, cy, cx;
      HPEN pen, open_;
      HBRUSH b, ob;
      COLORREF fill = (g_snap.mstate == MST_RUNNING) ? RGB(52,199,52)
                                                     : RGB(206,32,86);
      COLORREF ring = (g_th == &TH_DARK) ? RGB(150,150,160) : RGB(90,90,100);
      char cyc[48];
      RECT cr;

      if (g_snap.mstate == MST_RUNNING && g_snap.cycle_us > 0)
         snprintf(cyc, sizeof(cyc),
                  TR("同步周期 %.1f ms [%.1f~%.1f]",
                     "Sync cycle %.1f ms [%.1f~%.1f]"),
                  g_snap.cycle_us / 1000.0,
                  g_snap.cycle_min_us / 1000.0,
                  g_snap.cycle_max_us / 1000.0);
      else
         snprintf(cyc, sizeof(cyc), "%s", TR("同步周期 --", "Sync cycle --"));

      u2w(stx, ws, (int)(sizeof(ws) / sizeof(ws[0])));
      txw = GetTextExtentPoint32W(hdc, ws, (int)wcslen(ws), &sz) ? sz.cx : 100;
      cy = (tr.top + tr.bottom) / 2;
      cx = tr.right - txw - gap - d;      /* 指示灯圆心区左缘 */

      /* 周期文本（指示灯左侧，右对齐） */
      u2w(cyc, wc, (int)(sizeof(wc) / sizeof(wc[0])));
      if (GetTextExtentPoint32W(hdc, wc, (int)wcslen(wc), &sz))
         cr.right = cx - 10, cr.left = cr.right - sz.cx - 8;
      else
         cr.right = cx - 10, cr.left = cr.right - 140;
      cr.top = tr.top; cr.bottom = tr.bottom;
      text_out(hdc, cr, cyc, g_th->text_dim,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE, NULL);

      pen = CreatePen(PS_SOLID, 1, ring);
      b = CreateSolidBrush(fill);
      open_ = (HPEN)SelectObject(hdc, pen);
      ob = (HBRUSH)SelectObject(hdc, b);
      Ellipse(hdc, cx, cy - d / 2, cx + d, cy + d / 2);
      SelectObject(hdc, open_);
      SelectObject(hdc, ob);
      DeleteObject(pen);
      DeleteObject(b);
   }
}

/* ============================ 操作（弹窗/命令） ============================ */

/* 选择网卡弹窗：枚举网卡供用户选择，选中后启动主站线程 */
/* ============================ 通用选择窗口（标准对话框样式自绘弹窗） ============================ */
/* 代替系统 TrackPopupMenu 弹列表：自绘标题栏（放大/关闭按钮）、可拖动、
 * 列表带滚动条（滚轮 + 拖滑块）、悬停高亮、上次选择打勾、Esc 取消。
 * 配色固定为 Windows 标准对话框灰，与主窗口（跟随皮肤）形成父子区分。 */

#define PICK_CLASS    L"SimpleCATPick"
#define PICK_TITLE_H  36        /* 标题栏高度 */
#define PICK_ROW_H    30        /* 列表行高 */
#define PICK_SB_W     10        /* 滚动条宽度 */
#define PICK_MAX_ROWS 14        /* 不放大时最多可见行数 */

/* 标准对话框配色（不随主窗口皮肤变化，用于区分父子窗口） */
#define PICK_BG     RGB(240,240,240)   /* 背景（对话框灰） */
#define PICK_HEAD   RGB(225,225,225)   /* 标题栏（稍深灰） */
#define PICK_BORDER RGB(160,160,160)   /* 边框/分隔线 */
#define PICK_TEXT   RGB(32,32,32)      /* 正文 */
#define PICK_DIM    RGB(120,120,120)   /* 提示小字 */
#define PICK_HOVER  RGB(229,241,251)   /* 行悬停（系统列表蓝） */
#define PICK_SBTRK  RGB(228,228,228)   /* 滚动条轨道 */
#define PICK_BTNH   RGB(210,210,210)   /* 按钮悬停 */

typedef struct
{
   const char *title;
   char (*items)[280];
   int count;
   int mark;             /* 打勾行（如上次使用的网卡），-1 = 无 */
   int chosen;           /* 结果：>=0 = 选中行下标，-1 = 取消 */
   int maxed;            /* 1 = 已放大 */
   int scroll;           /* 首个可见行 */
   int hover;            /* 悬停行（绝对下标），-1 = 无 */
   int dragging;         /* 正在拖动滑块 */
   int grab;             /* 按下点相对滑块顶部的偏移 */
   int tracked;          /* TrackMouseEvent 已注册 */
   POINT mp;             /* 鼠标客户区坐标 */
   RECT norm;            /* 放大前的窗口矩形 */
} pick_t;

static pick_t *g_pick = NULL;

/* 按钮矩形（放大 | 关闭，位于标题栏右侧） */
static void pick_btn_rects(HWND h, RECT *mb, RECT *cb)
{
   RECT rc;
   GetClientRect(h, &rc);
   cb->left = rc.right - 46;
   cb->top = 7;
   cb->right = cb->left + 34;
   cb->bottom = PICK_TITLE_H - 7;
   mb->left = cb->left - 40;
   mb->top = cb->top;
   mb->right = mb->left + 34;
   mb->bottom = cb->bottom;
}

/* 列表区域矩形；*need_sb = 是否需要滚动条 */
static RECT pick_list_rect(HWND h, int *need_sb)
{
   RECT rc, lr;
   int vis;

   GetClientRect(h, &rc);
   vis = (rc.bottom - PICK_TITLE_H - 6 - 34) / PICK_ROW_H;
   if (vis < 1) vis = 1;
   *need_sb = (g_pick->count > vis);
   lr.left = 12;
   lr.right = rc.right - 12 - (*need_sb ? PICK_SB_W + 6 : 0);
   lr.top = PICK_TITLE_H + 6;
   lr.bottom = rc.bottom - 34;
   return lr;
}

static int pick_vis_rows(HWND h)
{
   RECT lr;
   int sb;
   int v;

   lr = pick_list_rect(h, &sb);
   v = (lr.bottom - lr.top) / PICK_ROW_H;
   return v < 1 ? 1 : v;
}

static void pick_clamp_scroll(HWND h)
{
   int maxs = g_pick->count - pick_vis_rows(h);

   if (maxs < 0) maxs = 0;
   if (g_pick->scroll > maxs) g_pick->scroll = maxs;
   if (g_pick->scroll < 0) g_pick->scroll = 0;
}

/* 轨道矩形 + 滑块矩形（调用方保证需要滚动条） */
static void pick_sb_rects(HWND h, RECT lr, RECT *trk, RECT *th)
{
   int vis = pick_vis_rows(h);
   int maxs = g_pick->count - vis;
   int hgt, thh;

   *trk = lr;
   trk->left = lr.right + 3;
   trk->right = trk->left + PICK_SB_W;
   *th = *trk;
   hgt = trk->bottom - trk->top;
   thh = (int)((double)vis * hgt / g_pick->count);
   if (thh < 20) thh = 20;
   if (thh > hgt) thh = hgt;
   if (maxs > 0)
      th->top = trk->top + (int)((double)(hgt - thh) * g_pick->scroll / maxs);
   else
      th->top = trk->top;
   th->bottom = th->top + thh;
}

/* 放大/还原切换 */
static void pick_toggle_max(HWND h)
{
   if (!g_pick->maxed)
   {
      RECT wa;
      GetWindowRect(h, &g_pick->norm);
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
      SetWindowPos(h, NULL, wa.left + 60, wa.top + 60,
                   wa.right - wa.left - 120, wa.bottom - wa.top - 120,
                   SWP_NOZORDER);
      g_pick->maxed = 1;
   }
   else
   {
      SetWindowPos(h, NULL,
                   g_pick->norm.left, g_pick->norm.top,
                   g_pick->norm.right - g_pick->norm.left,
                   g_pick->norm.bottom - g_pick->norm.top,
                   SWP_NOZORDER);
      g_pick->maxed = 0;
   }
   g_pick->scroll = 0;
   InvalidateRect(h, NULL, FALSE);
}

/* 画对勾（上次使用标记） */
static void pick_draw_check(HDC hdc, int cx, int cy, COLORREF c)
{
   HPEN pen = CreatePen(PS_SOLID, 2, c);
   HPEN op = (HPEN)SelectObject(hdc, pen);
   MoveToEx(hdc, cx - 5, cy, NULL);
   LineTo(hdc, cx - 1, cy + 4);
   LineTo(hdc, cx + 6, cy - 4);
   SelectObject(hdc, op);
   DeleteObject(pen);
}

static void pick_paint(HWND h)
{
   pick_t *p = g_pick;
   PAINTSTRUCT ps;
   HDC wdc = BeginPaint(h, &ps);
   RECT rc, lr, trk, th, r;
   int sb, vis, k;
   HDC mdc;
   HBITMAP bmp, ob;
   HBRUSH b;

   GetClientRect(h, &rc);
   mdc = CreateCompatibleDC(wdc);
   bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
   ob = (HBITMAP)SelectObject(mdc, bmp);

   /* 背景 + 标题栏 + 分隔线 */
   b = CreateSolidBrush(PICK_BG);
   FillRect(mdc, &rc, b);
   DeleteObject(b);
   r = rc;
   r.bottom = PICK_TITLE_H;
   b = CreateSolidBrush(PICK_HEAD);
   FillRect(mdc, &r, b);
   DeleteObject(b);
   r.top = PICK_TITLE_H;
   r.bottom = PICK_TITLE_H + 1;
   b = CreateSolidBrush(PICK_BORDER);
   FillRect(mdc, &r, b);
   DeleteObject(b);

   SetBkMode(mdc, TRANSPARENT);
   r = rc;
   r.left = 14;
   r.bottom = PICK_TITLE_H;
   text_out(mdc, r, p->title, PICK_TEXT,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font_bold);

   /* 标题栏按钮（放大 / 关闭） */
   {
      RECT mb, cb;
      pick_btn_rects(h, &mb, &cb);
      if (pt_in(mb, p->mp))
      {
         b = CreateSolidBrush(PICK_BTNH);
         FillRect(mdc, &mb, b);
         DeleteObject(b);
      }
      if (pt_in(cb, p->mp))
      {
         b = CreateSolidBrush(RGB(232,17,35));
         FillRect(mdc, &cb, b);
         DeleteObject(b);
      }
      /* 关闭：叉（悬停红底白叉，平时深灰） */
      {
         COLORREF xc = pt_in(cb, p->mp) ? RGB(255,255,255) : RGB(70,70,70);
         HPEN pen = CreatePen(PS_SOLID, 2, xc);
         HPEN op = (HPEN)SelectObject(mdc, pen);
         int cx = (cb.left + cb.right) / 2, cy = (cb.top + cb.bottom) / 2;
         MoveToEx(mdc, cx - 5, cy - 5, NULL);
         LineTo(mdc, cx + 5, cy + 5);
         MoveToEx(mdc, cx + 5, cy - 5, NULL);
         LineTo(mdc, cx - 5, cy + 5);
         SelectObject(mdc, op);
         DeleteObject(pen);
      }
      /* 放大：方框（放大状态画还原双层框） */
      {
         HPEN pen = CreatePen(PS_SOLID, 1, RGB(90,90,90));
         HPEN op = (HPEN)SelectObject(mdc, pen);
         HBRUSH obh = (HBRUSH)GetStockObject(NULL_BRUSH);
         int cx = (mb.left + mb.right) / 2, cy = (mb.top + mb.bottom) / 2;
         RECT g = { cx - 6, cy - 5, cx + 6, cy + 5 };
         HBRUSH ob2 = (HBRUSH)SelectObject(mdc, obh);
         FrameRect(mdc, &g, pen ? (HBRUSH)pen : obh);
         if (p->maxed)
         {
            RECT g2 = { cx - 3, cy - 2, cx + 3, cy + 2 };
            FrameRect(mdc, &g2, (HBRUSH)pen);
         }
         SelectObject(mdc, ob2);
         SelectObject(mdc, op);
         DeleteObject(pen);
      }
   }

   /* 列表行 */
   lr = pick_list_rect(h, &sb);
   vis = (lr.bottom - lr.top) / PICK_ROW_H;
   for (k = 0; k < vis; ++k)
   {
      int idx = p->scroll + k;
      RECT rr = lr;
      RECT tr;

      rr.top = lr.top + k * PICK_ROW_H;
      rr.bottom = rr.top + PICK_ROW_H - 4;
      if (idx >= p->count || rr.bottom > lr.bottom)
         break;

      if (idx == p->hover && idx != p->mark)
      {
         b = CreateSolidBrush(PICK_HOVER);
         FillRect(mdc, &rr, b);
         DeleteObject(b);
      }
      if (idx == p->mark)
         pick_draw_check(mdc, rr.left + 10, (rr.top + rr.bottom) / 2,
                         RGB(0,110,200));

      tr = rr;
      tr.left += (idx == p->mark) ? 26 : 14;
      tr.right -= 6;
      text_out(mdc, tr, p->items[idx], PICK_TEXT,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_font);
   }

   /* 滚动条 */
   if (sb)
   {
      pick_sb_rects(h, lr, &trk, &th);
      b = CreateSolidBrush(PICK_SBTRK);
      FillRect(mdc, &trk, b);
      DeleteObject(b);
      frame_rect_c(mdc, trk, PICK_BORDER);
      b = CreateSolidBrush((p->dragging) ? RGB(0,110,200) : PICK_BORDER);
      FillRect(mdc, &th, b);
      DeleteObject(b);
   }

   /* 底部提示 + 分隔线 */
   r = rc;
   r.top = rc.bottom - 31;
   r.bottom = r.top + 1;
   b = CreateSolidBrush(PICK_BORDER);
   FillRect(mdc, &r, b);
   DeleteObject(b);
   r.top = rc.bottom - 26;
   r.bottom = rc.bottom - 4;
   text_out(mdc, r, TR("点击选择   滚轮翻动   Esc 取消",
                       "Click to select   Scroll to move   Esc to cancel"),
            PICK_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE, g_font_small);

   /* 外边框 */
   frame_rect_c(mdc, rc, PICK_BORDER);

   BitBlt(wdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
   SelectObject(mdc, ob);
   DeleteObject(bmp);
   DeleteDC(mdc);
   EndPaint(h, &ps);
}

static LRESULT CALLBACK pick_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
   pick_t *p = g_pick;

   if (p == NULL)
      return DefWindowProcW(h, m, wp, lp);

   switch (m)
   {
      case WM_PAINT:
         pick_paint(h);
         return 0;
      case WM_ERASEBKGND:
         return 1;

      case WM_MOUSEMOVE:
      {
         POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
         RECT lr, trk, th;
         int sb, nh = -1, inval = 0;

         p->mp = pt;
         if (!p->tracked)
         {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme))
               p->tracked = 1;
         }

         lr = pick_list_rect(h, &sb);
         if (p->dragging && sb)
         {
            pick_sb_rects(h, lr, &trk, &th);
            {
               int vis = pick_vis_rows(h);
               int maxs = p->count - vis;
               int hgt = trk.bottom - trk.top;
               int thh = th.bottom - th.top;
               if (maxs > 0 && hgt > thh)
               {
                  int ny = pt.y - p->grab - trk.top;
                  int ns = (int)((double)ny / (hgt - thh) * maxs + 0.5);
                  if (ns < 0) ns = 0;
                  if (ns > maxs) ns = maxs;
                  if (ns != p->scroll)
                  {
                     p->scroll = ns;
                     inval = 1;
                  }
               }
            }
         }
         else if (pt_in(lr, pt))
         {
            int k = (pt.y - lr.top) / PICK_ROW_H;
            int idx = p->scroll + k;
            RECT rr = lr;
            rr.top = lr.top + k * PICK_ROW_H;
            rr.bottom = rr.top + PICK_ROW_H - 4;
            if (idx < p->count && rr.bottom <= lr.bottom)
               nh = idx;
         }

         if (nh != p->hover)
         {
            p->hover = nh;
            inval = 1;
         }
         if (inval)
            InvalidateRect(h, NULL, FALSE);
         return 0;
      }

      case WM_MOUSELEAVE:
         p->tracked = 0;
         p->hover = -1;
         p->mp.x = -100;
         p->mp.y = -100;
         InvalidateRect(h, NULL, FALSE);
         return 0;

      case WM_LBUTTONDOWN:
      {
         POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
         RECT mb, cb, lr, trk, th;
         int sb;

         if (pt.y < PICK_TITLE_H)
         {
            pick_btn_rects(h, &mb, &cb);
            if (pt_in(cb, pt))
            {
               p->chosen = -1;
               DestroyWindow(h);
               return 0;
            }
            if (pt_in(mb, pt))
            {
               pick_toggle_max(h);
               return 0;
            }
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);   /* 拖动窗口 */
            return 0;
         }

         lr = pick_list_rect(h, &sb);
         if (sb && pt.x >= lr.right + 2)
         {
            pick_sb_rects(h, lr, &trk, &th);
            if (pt_in(th, pt))
               p->grab = pt.y - th.top;
            else
               p->grab = (th.bottom - th.top) / 2;
            p->dragging = 1;
            SetCapture(h);
            SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
            return 0;
         }
         if (pt_in(lr, pt))
         {
            int k = (pt.y - lr.top) / PICK_ROW_H;
            int idx = p->scroll + k;
            RECT rr = lr;
            rr.top = lr.top + k * PICK_ROW_H;
            rr.bottom = rr.top + PICK_ROW_H - 4;
            if (idx < p->count && rr.bottom <= lr.bottom)
            {
               p->chosen = idx;
               DestroyWindow(h);
               return 0;
            }
         }
         return 0;
      }

      case WM_LBUTTONUP:
         if (p->dragging)
         {
            p->dragging = 0;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
         }
         return 0;

      case WM_MOUSEWHEEL:
      {
         int d = (short)HIWORD(wp);
         p->scroll += (d < 0) ? 3 : -3;
         pick_clamp_scroll(h);
         InvalidateRect(h, NULL, FALSE);
         return 0;
      }

      case WM_KEYDOWN:
         if (wp == VK_ESCAPE)
         {
            p->chosen = -1;
            DestroyWindow(h);
            return 0;
         }
         break;

      case WM_DESTROY:
         g_pick = NULL;
         return 0;
   }
   return DefWindowProcW(h, m, wp, lp);
}

/* 模态弹出选择窗口；返回选中行下标，-1 = 取消。
 * items: 每项最多 280 字符；mark: 打勾行（-1 无） */
static int pick_show(const char *title, char items[][280], int count, int mark)
{
   pick_t st;
   static int cls_ok = 0;
   WNDCLASSEXW wc;
   RECT orr;
   int w = 560, h, rows, x, y;
   MSG msg;

   if (g_pick != NULL || count <= 0)
      return -1;

   memset(&st, 0, sizeof(st));
   st.title = title;
   st.items = items;
   st.count = count;
   st.mark = mark;
   st.chosen = -1;
   st.hover = -1;
   st.mp.x = -100;
   st.mp.y = -100;
   g_pick = &st;

   if (!cls_ok)
   {
      memset(&wc, 0, sizeof(wc));
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = pick_proc;
      wc.hInstance = GetModuleHandleW(NULL);
      wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
      wc.lpszClassName = PICK_CLASS;
      RegisterClassExW(&wc);
      cls_ok = 1;
   }

   rows = count > PICK_MAX_ROWS ? PICK_MAX_ROWS : count;
   h = PICK_TITLE_H + 6 + rows * PICK_ROW_H + 40;
   if (h < 260) h = 260;
   GetWindowRect(g_hwnd, &orr);
   x = orr.left + ((orr.right - orr.left) - w) / 2;
   y = orr.top + 80;

   EnableWindow(g_hwnd, FALSE);        /* 模态：屏蔽主窗口 */
   {
      HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW, PICK_CLASS, L"",
                                WS_POPUP | WS_VISIBLE,
                                x, y, w, h, g_hwnd, NULL,
                                GetModuleHandleW(NULL), NULL);
      SetForegroundWindow(hw);
      SetFocus(hw);
      SetCapture(hw);
      ReleaseCapture();                /* 确保立即收到 mouse move */
      while (g_pick != NULL && GetMessageW(&msg, NULL, 0, 0) > 0)
      {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }
   EnableWindow(g_hwnd, TRUE);
   SetActiveWindow(g_hwnd);
   g_pick = NULL;
   return st.chosen;
}

/* ---------------- CiA402 驱动控制（交互） ---------------- */

/* 点击状态行：弹出 CiA402 协议与操作说明（新手向导） */
static void c402_info_show(void)
{
   msgbox_utf8(
      TR("CiA402（IEC 61800-5-2）是伺服驱动器的标准设备协议，"
         "EtherCAT 通过 CoE（CANopen over EtherCAT）承载它。\n\n"
         "主站把【控制字 0x6040】下发给驱动，驱动用【状态字 0x6041】"
         "汇报当前状态。驱动内部有一个状态机：\n\n"
         "    未使能 → 上电就绪 → 已开通 → 运行使能\n\n"
         "电机只有在【运行使能】状态下才会响应位置/速度/力矩指令。\n\n"
         "日常操作：\n"
         "  · 点【一键使能】：自动完成 上电就绪→开通→运行使能；\n"
         "  · 出现故障先点【故障复位】，再重新一键使能；\n"
         "  · 紧急情况点【快速停止】或【禁止电压】。\n\n"
         "运行模式（0x6060 设置 / 0x6061 显示）决定驱动如何解释目标值：\n"
         "CSP=周期同步位置，CSV=周期同步速度，CST=周期同步力矩。\n"
         "点击\"运行模式\"一行可切换模式；目标值（如目标位置 0x607A）\n"
         "直接在上方变量树里点击对应变量输入即可。",
         "CiA402 (IEC 61800-5-2) is the standard drive profile for servo"
         " drives, carried over EtherCAT via CoE (CANopen over EtherCAT).\n\n"
         "The master writes the [controlword 0x6040] to the drive; the drive"
         " reports its state with the [statusword 0x6041]. The drive has an"
         " internal state machine:\n\n"
         "    Switch-on disabled -> Ready to switch on -> Switched on"
         " -> Operation enabled\n\n"
         "The motor only responds to position/velocity/torque commands"
         " in [Operation enabled] state.\n\n"
         "Daily use:\n"
         "  - Click [Auto Enable]: runs shutdown -> switch on -> enable;\n"
         "  - On a fault, click [Fault Reset], then auto-enable again;\n"
         "  - In an emergency click [Quick Stop] or [Disable Voltage].\n\n"
         "The mode of operation (0x6060 set / 0x6061 display) defines how"
         " the drive interprets setpoints: CSP = cyclic sync position,"
         " CSV = cyclic sync velocity, CST = cyclic sync torque.\n"
         "Click the \"Mode\" row to change it; write setpoints (e.g. target"
         " position 0x607A) directly in the variable tree above."),
      TR("CiA402 协议说明", "About CiA402"), MB_OK | MB_ICONINFORMATION);
}

/* 点击控制按钮：下发 UICMD_CIA402 并记一条消息 */
static void c402_btn_click(int slave, int act)
{
   const char *aname = "";
   char logtxt[160];
   ui_cmd_msg_t cmd;

   if (act <= 0) return;
   if (slave < 1 || slave > g_snap.slavecount) return;

   if (g_snap.mstate != MST_RUNNING)
   {
      msgbox_utf8(TR("请先连接总线（扫描新的从站）后再控制驱动。",
                     "Connect the bus (Scan New Slaves) before controlling drives."),
                  TR("CiA402 驱动控制", "CiA402 Drive Control"), MB_ICONINFORMATION);
      return;
   }

   memset(&cmd, 0, sizeof(cmd));
   cmd.id = UICMD_CIA402;
   cmd.slave = slave;
   cmd.var_index = act;
   cmd.value = 0;
   ui_master_post(&cmd);

   switch (act)
   {
      case C402_ACT_AUTOENABLE: aname = TR("一键使能", "auto-enable");      break;
      case C402_ACT_RESET:      aname = TR("故障复位", "fault reset");      break;
      case C402_ACT_QSTOP:      aname = TR("快速停止", "quick stop");       break;
      case C402_ACT_SHUTDOWN:   aname = TR("上电就绪", "shutdown");         break;
      case C402_ACT_SWITCHON:   aname = TR("开通", "switch on");            break;
      case C402_ACT_DISABLE:    aname = TR("关使能", "disable operation");  break;
      case C402_ACT_DISVOLT:    aname = TR("禁止电压", "disable voltage");  break;
      default: break;
   }
   snprintf(logtxt, sizeof(logtxt), TR("从站 %d：下发 %s 命令", "Slave %d: %s sent"),
            slave, aname);
   log_push(logtxt);
}

/* 点击模式行：弹出运行模式选择（CiA402 标准模式码） */
static void c402_mode_click(int slave)
{
   static const int modes[] = { 8, 9, 10, 1, 2, 3, 4 };
   static const int nmodes = (int)(sizeof(modes) / sizeof(modes[0]));
   static char items[8][280];
   static char ttl[128];
   const ui_slave_info_t *s;
   int k, id, mark = -1;

   if (slave < 1 || slave > g_snap.slavecount) return;
   s = &g_snap.slave[slave];

   if (g_snap.mstate != MST_RUNNING)
   {
      msgbox_utf8(TR("请先连接总线（扫描新的从站）后再切换运行模式。",
                     "Connect the bus (Scan New Slaves) before changing modes."),
                  TR("CiA402 驱动控制", "CiA402 Drive Control"), MB_ICONINFORMATION);
      return;
   }
   if (s->c402_mode < 0)
   {
      msgbox_utf8(TR("该从站的过程数据中没有 0x6060/0x6061 运行模式对象，无法切换。",
                     "This slave's process data has no 0x6060/0x6061 mode objects."),
                  TR("CiA402 驱动控制", "CiA402 Drive Control"), MB_ICONINFORMATION);
      return;
   }

   for (k = 0; k < nmodes; ++k)
   {
      snprintf(items[k], sizeof(items[k]), "%d - %s", modes[k], c402_mode_name(modes[k]));
      if (s->c402_mode == modes[k]) mark = k;
   }
   snprintf(ttl, sizeof(ttl), TR("从站 %d：选择 CiA402 运行模式",
                                 "Slave %d: select CiA402 mode of operation"), slave);
   id = pick_show(ttl, items, nmodes, mark);
   if (id >= 0 && id < nmodes)
   {
      ui_cmd_msg_t cmd;
      char logtxt[160];

      memset(&cmd, 0, sizeof(cmd));
      cmd.id = UICMD_CIA402;
      cmd.slave = slave;
      cmd.var_index = C402_ACT_MODE;
      cmd.value = modes[id];
      ui_master_post(&cmd);
      snprintf(logtxt, sizeof(logtxt), TR("从站 %d：运行模式切换为 %s",
                                          "Slave %d: mode changed to %s"),
               slave, c402_mode_name(modes[id]));
      log_push(logtxt);
   }
}

static void do_select_adapter(void)
{
   ec_adaptert *head, *a;
   static char items[64][280];
   int n = 0, id, i = 0, k, mark = -1;

   head = a = master_list_adapters(&n);
   if (n == 0)
   {
      msgbox_utf8(TR("未找到网卡！请确认已安装 WinPcap/Npcap 且网卡已启用。",
                     "No NIC found! Make sure WinPcap/Npcap is installed and the NIC is enabled."),
                  TR("选择网卡", "Select NIC"), MB_ICONWARNING);
      if (head) ec_free_adapters(head);
      return;
   }
   for (a = head; a != NULL && i < 64; a = a->next, ++i)
   {
      snprintf(items[i], sizeof(items[i]), "%s", a->desc[0] ? a->desc : a->name);
      if (g_cfg.last_adapter[0] != '\0' && strcmp(a->name, g_cfg.last_adapter) == 0)
         mark = i;                      /* 上次使用的网卡打勾 */
   }

   id = pick_show(TR("选择网卡（EtherCAT 总线接口）",
                     "Select NIC (EtherCAT bus interface)"), items, i, mark);

   if (id >= 0 && id < i)
   {
      a = head;
      for (k = 0; k < id && a != NULL; ++k)
         a = a->next;
      if (a != NULL)
      {
         char logtxt[300];
         strncpy(g_cfg.last_adapter, a->name, sizeof(g_cfg.last_adapter) - 1);
         g_cfg.last_adapter[sizeof(g_cfg.last_adapter) - 1] = '\0';
         config_save_now();          /* 记住上次用的网卡 */
         ui_master_start(a->name); /* 内部会先停止旧线程 */
         chan_clear_all();
         g_tree_scroll = 0;
         tree_reset_expand();
         snprintf(logtxt, sizeof(logtxt), TR("选择网卡: %s", "NIC selected: %s"),
                  a->desc[0] ? a->desc : a->name);
         log_push(logtxt);
      }
   }
   ec_free_adapters(head);
}

/* "扫描新的从站"：未连接时先弹出网卡列表建立连接，已连接则请求完整重扫 */
static void do_scan_or_connect(void)
{
   if (g_snap.mstate == MST_RUNNING || g_snap.mstate == MST_STARTFAIL)
   {
      ui_cmd_msg_t cmd; memset(&cmd, 0, sizeof(cmd));
      cmd.id = UICMD_RESCAN;
      ui_master_post(&cmd);
      log_push(TR("请求重新扫描总线", "Bus rescan requested"));
   }
   else if (g_snap.mstate == MST_IDLE)
   {
      do_select_adapter();
   }
}

/* 烧录 XML：列出 xml/ 下文件供选择，再选目标从站，确认后提交主站线程 */
static void do_flash_xml(void)
{
   char files[ESI_FLASH_MAX_FILES][ESI_FLASH_NAME_MAX];
   static char items[ESI_FLASH_MAX_FILES][280];
   static char sitems[MASTER_MAXSLAVE + 1][280];
   int n, i, id, sn = 0, s;

   if (g_snap.mstate != MST_RUNNING && g_snap.mstate != MST_STARTFAIL)
   {
      msgbox_utf8(TR("请先连接网卡（扫描新的从站）后再烧录。",
                     "Connect a NIC (Scan New Slaves) before flashing."),
                  TR("烧录XML文件", "Flash XML File"), MB_ICONINFORMATION);
      return;
   }
   if (g_snap.flash_phase != 0)
   {
      msgbox_utf8(TR("烧录流程正在进行中。", "A flash operation is already running."),
                  TR("烧录XML文件", "Flash XML File"), MB_ICONINFORMATION);
      return;
   }
   n = esi_flash_list_xml("xml", files, ESI_FLASH_MAX_FILES);
   if (n == 0)
   {
      msgbox_utf8(TR("xml 目录下没有可解析的 .xml 设备描述文件。",
                     "No parseable .xml device description files in the xml directory."),
                  TR("烧录XML文件", "Flash XML File"),
                  MB_ICONWARNING);
      return;
   }
   for (i = 0; i < n; ++i)
   {
      snprintf(items[i], sizeof(items[i]), "%s", files[i]);
      items[i][sizeof(items[i]) - 1] = '\0';
   }
   id = pick_show(TR("选择要烧录的 XML 设备描述文件",
                     "Select the XML device description to flash"), items, n, -1);
   if (id < 0 || id >= n)
      return;

   /* 第二步：选择烧录目标从站（多从站时避免只能烧到 1 号从站） */
   if (g_snap.slavecount <= 0)
   {
      msgbox_utf8(TR("总线上没有检测到从站，无法烧录。请先扫描从站。",
                     "No slave detected on the bus, cannot flash. Scan the bus first."),
                  TR("烧录XML文件", "Flash XML File"),
                  MB_ICONWARNING);
      return;
   }
   for (i = 1; i <= g_snap.slavecount && i <= MASTER_MAXSLAVE; ++i)
   {
      const ui_slave_info_t *sl = &g_snap.slave[i];
      snprintf(sitems[i - 1], sizeof(sitems[i - 1]), TR("从站 %d    %s    [%s]",
                                                        "Slave %d    %s    [%s]"),
               i, sl->name[0] ? sl->name : TR("(未知从站)", "(unknown slave)"), sl->label);
      sitems[i - 1][sizeof(sitems[i - 1]) - 1] = '\0';
   }
   if (g_cfg.last_flash_slave >= 1 && g_cfg.last_flash_slave <= g_snap.slavecount)
      s = g_cfg.last_flash_slave - 1;   /* 上次烧录的从站默认选中 */
   else
      s = 0;
   s = pick_show(TR("选择要烧录到哪个从站", "Select the target slave to flash"),
                 sitems, g_snap.slavecount, s);
   if (s < 0 || s >= g_snap.slavecount)
      return;
   sn = s + 1;                          /* 从站序号（1 起） */

   {
      char confirm[640];
      snprintf(confirm, sizeof(confirm),
               TR("即将把 %s 烧录到【从站 %d】的 EEPROM！\n\n"
                  "目标从站：%s\n"
                  "将重写：身份区(厂商/产品/修订版本) + 类别区(STRINGS/GENERAL/FMMU/SM/PDO)。\n"
                  "不碰：PDI 配置 + CRC 校验区与序列号。\n"
                  "烧录后需给该从站重新上电生效。\n\n确认烧录？",
                  "About to flash %s into the EEPROM of [Slave %d]!\n\n"
                  "Target slave: %s\n"
                  "Rewrites: identity area (vendor/product/revision)"
                  " + categories (STRINGS/GENERAL/FMMU/SM/PDO).\n"
                  "Untouched: PDI config + CRC area and serial number.\n"
                  "Power-cycle the slave afterwards to take effect.\n\nFlash now?"),
               files[id], sn,
               g_snap.slave[sn].name[0] ? g_snap.slave[sn].name : TR("(未知从站)", "(unknown slave)"));
      if (msgbox_utf8(confirm, TR("确认烧录", "Confirm Flash"),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
      {
         ui_cmd_msg_t cmd;
         char logtxt[200];
         memset(&cmd, 0, sizeof(cmd));
         cmd.id = UICMD_FLASH_FILE;
         cmd.slave = sn;
         strncpy(cmd.name, files[id], sizeof(cmd.name) - 1);
         ui_master_post(&cmd);
         g_cfg.last_flash_slave = sn;
         config_save_now();             /* 记住上次烧录的从站 */
         snprintf(logtxt, sizeof(logtxt), TR("开始烧录 %s → 从站 %d",
                                             "Flashing %s -> slave %d"), cmd.name, sn);
         log_push(logtxt);
      }
   }
}

/* 应用从站模式：一直记录偏好（下次启动沿用），已连接则立即下发切换 */
static void apply_slave_mode(int dc_on)
{
   ui_master_set_mode_pref(dc_on);
   if (g_snap.mstate != MST_IDLE && g_snap.mstate != MST_STARTFAIL)
   {
      ui_cmd_msg_t cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.id = dc_on ? UICMD_MODE_DC : UICMD_MODE_FREERUN;
      ui_master_post(&cmd);
   }
   else
      log_push(dc_on ? TR("从站模式已设为 DC（连接总线后生效，已保存）",
                          "Slave mode set to DC (takes effect on connect, saved)")
                     : TR("从站模式已设为 FreeRUN（连接总线后生效，已保存）",
                          "Slave mode set to FreeRUN (takes effect on connect, saved)"));
   config_save_now();
}

/* "设置从站模式"弹窗：DC 同步 / FreeRUN */
static void do_slave_mode(void)
{
   HMENU m = new_popup();
   int id, cur_dc;
   POINT p;
   RECT wr;

   /* 已连接看实际状态；未连接看保存的偏好（-1 视为自动 = FreeRUN） */
   cur_dc = (g_snap.mstate == MST_IDLE || g_snap.mstate == MST_STARTFAIL)
            ? (ui_master_get_mode_pref() == 1) : (g_snap.dc_active != 0);

   menu_item_utf8(m, IDM_DC, TR("DC 模式（分布式时钟同步）",
                                "DC mode (distributed clocks)"), cur_dc ? MF_CHECKED : 0);
   menu_item_utf8(m, IDM_FREERUN, TR("FreeRUN 模式", "FreeRUN mode"), cur_dc ? 0 : MF_CHECKED);
   GetWindowRect(g_hwnd, &wr);
   p.x = wr.left + 60; p.y = wr.top + 200;
   id = popup_menu(m, g_hwnd, p);
   if (id == IDM_DC)
      apply_slave_mode(1);
   else if (id == IDM_FREERUN)
      apply_slave_mode(0);
}

/* ============================ 输出变量就地编辑 ============================ */

static void editval_commit(int apply)
{
   wchar_t wv[32];
   char val[32];
   int idx;

   if (g_editing_idx < 0) return;
   idx = g_editing_idx;
   g_editing_idx = -1;
   ShowWindow(g_edit_val, SW_HIDE);

   if (apply)
   {
      long value;
      ui_cmd_msg_t cmd;
      char logtxt[192];

      GetWindowTextW(g_edit_val, wv, 32);
      WideCharToMultiByte(CP_UTF8, 0, wv, -1, val, sizeof(val), NULL, NULL);
      trim_inplace(val);
      if (val[0] == '\0') return;
      value = strtol(val, NULL, 0);
      memset(&cmd, 0, sizeof(cmd));
      cmd.id = UICMD_WRITE_VAR;
      cmd.var_index = idx;
      cmd.value = value;
      ui_master_post(&cmd);
      snprintf(logtxt, sizeof(logtxt), TR("写入 %s = %ld（请求已发送，结果见消息栏）",
                                          "Write %s = %ld (request sent, see message bar)"),
               g_snap.vl.var[idx].name, value);
      log_push(logtxt);
   }
   InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK editval_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
   switch (m)
   {
      case WM_GETDLGCODE:
         /* 关键：主消息循环用 IsDialogMessageW 分发按键，会把回车/ESC 吞掉
            转成 IDOK/IDCANCEL 发给主窗口。在此声明编辑框要自己处理这些键，
            回车（发送）和 ESC（取消）才能到达下面的 WM_KEYDOWN */
         return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
      case WM_KEYDOWN:
         if (wp == VK_RETURN) { editval_commit(1); return 0; }
         if (wp == VK_ESCAPE) { editval_commit(0); return 0; }
         break;
      case WM_CHAR:   /* 兜底：某些键盘状态/IME 组合下只收到字符消息 */
         if (wp == '\r' || wp == '\n') { editval_commit(1); return 0; }
         if (wp == 27)                 { editval_commit(0); return 0; }
         break;
   }
   return CallWindowProcW(g_edit_oldproc, h, m, wp, lp);
}

/* ============================ 波形缓冲区上限设置 ============================ */

/* 把编辑框文本刷新为当前生效值 */
static void capedit_sync_text(void)
{
   wchar_t wv[32];
   _snwprintf(wv, 32, L"%d", g_wave_cap);
   wv[31] = L'\0';
   SetWindowTextW(g_edit_cap, wv);
}

/* 回车/失焦提交：解析并应用新上限 */
static void capedit_commit(void)
{
   wchar_t wv[32];
   char val[32];
   int cap;

   GetWindowTextW(g_edit_cap, wv, 32);
   WideCharToMultiByte(CP_UTF8, 0, wv, -1, val, sizeof(val), NULL, NULL);
   trim_inplace(val);
   cap = atoi(val);
   if (cap >= WAVE_CAP_MIN && cap <= WAVE_CAP_MAX)
      wave_cap_apply(cap);
   capedit_sync_text();   /* 非法输入或未变化都恢复显示实际值 */
}

static LRESULT CALLBACK capedit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
   switch (m)
   {
      case WM_GETDLGCODE:   /* 主循环 IsDialogMessageW 会吞回车，在此声明自己处理 */
         return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
      case WM_KEYDOWN:
         if (wp == VK_RETURN) { capedit_commit(); return 0; }
         if (wp == VK_ESCAPE) { capedit_sync_text(); return 0; }
         break;
      case WM_CHAR:
         if (wp == '\r' || wp == '\n') { capedit_commit(); return 0; }
         if (wp == 27)                 { capedit_sync_text(); return 0; }
         break;
      case WM_KILLFOCUS:      /* 点别处失焦时若内容有变化也提交 */
         capedit_commit();
         break;
   }
   return CallWindowProcW(g_cap_oldproc, h, m, wp, lp);
}

/* 在指定变量数值框处开始编辑 */
static void editval_begin(HWND hwnd, const RECT *val, int var_index)
{
   wchar_t wv[32];
   if (g_snap.vl.var[var_index].dir != 0) return; /* 仅输出变量可写 */
   g_editing_idx = var_index;
   g_edit_begin_tick = GetTickCount();
   _snwprintf(wv, 32, L"%lld", g_snap.oval[var_index]);
   wv[31] = L'\0';
   MoveWindow(g_edit_val, val->left, val->top,
              val->right - val->left, val->bottom - val->top, TRUE);
   SetWindowTextW(g_edit_val, wv);
   ShowWindow(g_edit_val, SW_SHOW);
   SetFocus(g_edit_val);
   SendMessageW(g_edit_val, EM_SETSEL, 0, -1);
}

/* ============================ 窗口过程 ============================ */

/* 烧录相关的界面联动：身份不符确认 + 结果提示。
   故意与"暂停刷新"解耦——暂停只是冻结数值/波形/日志，
   烧录是一次性用户操作，进度与结果必须始终有反馈。 */
static void flash_ui_poll(void)
{
   /* 身份不符：弹确认框（只弹一次） */
   if (g_snap.flash_phase == 2 && !g_flash_wait_shown)
   {
      char txt[400];
      g_flash_wait_shown = 1;
      snprintf(txt, sizeof(txt),
               TR("[身份不匹配] %s\n\n换装新 ESI 时身份本来就不会一致"
                  "（TwinCAT 同样允许给从站写任意 ESI）。\n"
                  "继续将把该 XML 的身份区 + 类别区写入从站 EEPROM，"
                  "烧录后从站即成为该 XML 描述的设备（需重新上电生效）。\n\n"
                  "强制继续烧录？",
                  "[Identity mismatch] %s\n\nIdentities are naturally different"
                  " when installing a new ESI"
                  " (TwinCAT likewise allows writing any ESI to a slave).\n"
                  "Continuing will write this XML's identity + categories"
                  " into the slave's EEPROM;"
                  " the slave then becomes the device this XML describes"
                  " (power-cycle needed).\n\n"
                  "Force flashing anyway?"),
               g_snap.flash_mismatch_msg);
      if (msgbox_utf8(txt, TR("身份不匹配", "Identity mismatch"),
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
      {
         ui_cmd_msg_t cmd; memset(&cmd, 0, sizeof(cmd));
         cmd.id = UICMD_FLASH_YES;
         ui_master_post(&cmd);
      }
      else
      {
         ui_cmd_msg_t cmd; memset(&cmd, 0, sizeof(cmd));
         cmd.id = UICMD_FLASH_NO;
         ui_master_post(&cmd);
      }
   }
   if (g_snap.flash_phase != 2)
      g_flash_wait_shown = 0;

   /* 烧录结果提示 */
   if (g_snap.flash_seq != g_last_flash_seq && g_snap.flash_result != 0)
   {
      g_last_flash_seq = g_snap.flash_seq;
      if (g_snap.flash_result > 0)
      {
         char txt[300];
         snprintf(txt, sizeof(txt),
                  TR("烧录完成！已写入【从站 %d】的 EEPROM。\n\n"
                     "请给该从站重新上电，主站会自动重新检测并匹配新 ESI。",
                     "Flash done! Written to the EEPROM of [Slave %d].\n\n"
                     "Power-cycle the slave; the master will then"
                     " re-detect it and match the new ESI."),
                  g_snap.flash_slave);
         msgbox_utf8(txt, TR("烧录结果", "Flash Result"), MB_OK | MB_ICONINFORMATION);
      }
      else
         msgbox_utf8(g_snap.flash_err, TR("烧录失败", "Flash Failed"),
                     MB_OK | MB_ICONERROR);
   }
}

/* 从主站快照中只同步烧录字段（暂停刷新时用，保证数值/波形/日志仍冻结） */
static void flash_fields_sync(void)
{
   ui_snap_t fs;

   ui_master_get_snap(&fs);
   g_snap.flash_phase        = fs.flash_phase;
   g_snap.flash_done         = fs.flash_done;
   g_snap.flash_total        = fs.flash_total;
   g_snap.flash_slave        = fs.flash_slave;
   g_snap.flash_result       = fs.flash_result;
   g_snap.flash_seq          = fs.flash_seq;
   memcpy(g_snap.flash_file, fs.flash_file, sizeof(g_snap.flash_file));
   memcpy(g_snap.flash_mismatch_msg, fs.flash_mismatch_msg,
          sizeof(g_snap.flash_mismatch_msg));
   memcpy(g_snap.flash_err, fs.flash_err, sizeof(g_snap.flash_err));
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   static layout_t L;
   static int W = 0, H = 0;

   switch (msg)
   {
      case WM_CREATE:
      {
         HINSTANCE hi = ((LPCREATESTRUCTW)lp)->hInstance;
         g_hwnd = hwnd;

         g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
         g_font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                    0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
         g_font_bold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                   0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
         g_font_title = CreateFontW(-17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                    0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

         /* 输出变量数值就地编辑框（隐藏，点击数值框时出现）
            无边框 + 与行内数值同字体，配合 WM_CTLCOLOREDIT 高亮底色，
            编辑时不显示突兀的矩形框，只有数值部分被高亮 */
         g_edit_val = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                    0, 0, 64, 18, hwnd, (HMENU)95, hi, NULL);
         SendMessageW(g_edit_val, WM_SETFONT, (WPARAM)g_font, TRUE);
         g_edit_oldproc = (WNDPROC)SetWindowLongPtrW(g_edit_val, GWLP_WNDPROC,
                                                     (LONG_PTR)editval_proc);

         /* 波形缓冲区上限编辑框（波形面板底部，数字输入，回车/失焦生效） */
         g_edit_cap = CreateWindowW(L"EDIT", L"", WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL,
                                    0, 0, 76, 20, hwnd, (HMENU)96, hi, NULL);
         SendMessageW(g_edit_cap, WM_SETFONT, (WPARAM)g_font_small, TRUE);
         g_cap_oldproc = (WNDPROC)SetWindowLongPtrW(g_edit_cap, GWLP_WNDPROC,
                                                    (LONG_PTR)capedit_proc);

         SetTimer(hwnd, 1, 20, NULL);   /* 20ms 基准定时 */
         return 0;
      }

      case WM_SIZE:
      {
         W = LOWORD(lp); H = HIWORD(lp);
         layout_calc(&L, W, H);
         return 0;
      }

      case WM_TIMER:
      {
         /* 暂停刷新：快照/波形/日志全部冻结，仅重绘界面状态。
            主站后台线程继续 1ms 周期通信，恢复后直接显示最新数据。
            例外：烧录是一次性交互操作，其进度条与结果提示必须继续刷新，
            否则暂停时看不到进度、烧完也不弹提示。 */
         if (g_paused)
         {
            flash_fields_sync();
            flash_ui_poll();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
         }

         ui_master_get_snap(&g_snap);

#ifdef UI_TEST_TREE   /* 无硬件验证：变量数与绘制注入保持一致 */
         if (g_snap.vl.count == 0)
         {
            int v;
            g_snap.vl.count = 60;
            for (v = 0; v < 60; ++v)
            {
               strcpy(g_snap.vl.var[v].name, "TestVar");
               g_snap.vl.var[v].index = 0x6000 + v / 4;
               g_snap.vl.var[v].dir = v % 2;
               g_snap.vl.var[v].slave = (uint8)(v % 3) + 1;
            }
         }
#endif

         /* 波形采样已移至独立线程 wave_sampler（支持 1ms 周期） */

         /* 主站消息 -> 日志（按序号增量拉取，快速连续的消息不丢） */
         {
            ui_msgitem_t items[16];
            unsigned next = g_last_msg_seq;
            int nm = ui_master_pull_msgs(g_last_msg_seq, items, 16, &next);
            if (nm > 0)
            {
               int kk;
               for (kk = 0; kk < nm; ++kk)
                  log_push_ex(items[kk].text, msg_is_error(items[kk].text));
               g_last_msg_seq = next;
            }
         }

         /* 重扫/重连后变量数变化：清理失效通道与编辑框 */
         {
            int c = 0;
            EnterCriticalSection(&g_wave_cs);
            while (c < g_chan_count)
            {
               if (g_chan[c].var >= g_snap.vl.count)
                  chan_remove_at(c);      /* 含缓冲指针所有权处理 */
               else c++;
            }
            LeaveCriticalSection(&g_wave_cs);
         }
         if (g_editing_idx >= 0 &&
             (g_editing_idx >= g_snap.vl.count ||
              g_snap.vl.var[g_editing_idx].dir != 0))
            editval_commit(0);

         /* 烧录状态联动：进度条随快照刷新，确认框/结果提示在此处理 */
         flash_ui_poll();

         InvalidateRect(hwnd, NULL, FALSE);
         return 0;
      }

      case WM_PAINT:
      {
         PAINTSTRUCT ps;
         HDC hdc = BeginPaint(hwnd, &ps);
         HDC mem;
         HBITMAP bmp, obmp;
         HFONT of;
         POINT mp;
         RECT full;

         GetCursorPos(&mp);
         ScreenToClient(hwnd, &mp);

         mem = CreateCompatibleDC(hdc);
         bmp = CreateCompatibleBitmap(hdc, W > 0 ? W : 1, H > 0 ? H : 1);
         obmp = (HBITMAP)SelectObject(mem, bmp);

         full.left = 0; full.top = 0; full.right = W; full.bottom = H;
         fill_rect_c(mem, full, g_th->bg);

         of = (HFONT)SelectObject(mem, g_font);
         SetBkMode(mem, TRANSPARENT);

         draw_topbar(mem, &L.top, mp);
         draw_left(mem, &L.left, mp);
         draw_tree(mem, &L.right, mp);
         draw_wave(mem, &L.wave, mp);
         draw_log(mem, &L.log, mp);
         tree_tooltip_paint(mem);   /* 悬停浮框最后画：压在所有面板之上 */

         SelectObject(mem, of);
         BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
         SelectObject(mem, obmp);
         DeleteObject(bmp);
         DeleteDC(mem);
         EndPaint(hwnd, &ps);
         return 0;
      }

      case WM_ERASEBKGND:
         return 1;

      case WM_LBUTTONDOWN:
      {
         POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
         SetFocus(hwnd);

         /* 顶栏菜单 */
         if (p.y < TOP_H)
         {
            RECT m1, m2, m3, m4;
            topbar_menu_rects(&m1, &m2, &m3, &m4);
            if (pt_in(m1, p))
            {
               HMENU m = new_popup();
               menu_item_utf8(m, IDM_NIC, TR("选择网卡...", "Select NIC..."), 0);
               menu_item_utf8(m, IDM_FLASH, TR("烧录XML到EEPROM...", "Flash XML to EEPROM..."),
                              g_snap.mstate == MST_IDLE ? MF_GRAYED : 0);
               menu_item_utf8(m, IDM_OPXMLDIR, TR("打开 xml 目录", "Open xml directory"), 0);
               AppendMenuW(m, MF_SEPARATOR, 0, NULL);
               menu_item_utf8(m, IDM_EXIT, TR("退出", "Exit"), 0);
               { POINT sp = p; ClientToScreen(hwnd, &sp);
                 int id = popup_menu(m, hwnd, sp);
                 PostMessageW(hwnd, WM_COMMAND, id, 0); }
            }
            else if (pt_in(m2, p))
            {
               HMENU m = new_popup();
               char lab[48];
               int cur_dc = (g_snap.mstate == MST_IDLE || g_snap.mstate == MST_STARTFAIL)
                            ? (ui_master_get_mode_pref() == 1) : (g_snap.dc_active != 0);
               menu_item_utf8(m, IDM_DC, TR("DC 模式（分布式时钟同步）",
                                            "DC mode (distributed clocks)"),
                              cur_dc ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_FREERUN, TR("FreeRUN 模式", "FreeRUN mode"),
                              cur_dc ? 0 : MF_CHECKED);
               AppendMenuW(m, MF_SEPARATOR, 0, NULL);
               snprintf(lab, sizeof(lab), TR("波形采样 1 ms", "Wave sampling 1 ms"));
               menu_item_utf8(m, IDM_SAMP_BASE + 7, lab, g_sample_ms == 1 ? MF_CHECKED : 0);
               snprintf(lab, sizeof(lab), TR("波形采样 5 ms", "Wave sampling 5 ms"));
               menu_item_utf8(m, IDM_SAMP_BASE + 6, lab, g_sample_ms == 5 ? MF_CHECKED : 0);
               snprintf(lab, sizeof(lab), TR("波形采样 10 ms", "Wave sampling 10 ms"));
               menu_item_utf8(m, IDM_SAMP_BASE + 5, lab, g_sample_ms == 10 ? MF_CHECKED : 0);
               snprintf(lab, sizeof(lab), TR("波形采样 50 ms", "Wave sampling 50 ms"));
               menu_item_utf8(m, IDM_SAMP_BASE + 1, lab, g_sample_ms == 50 ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_SAMP_BASE + 2, TR("波形采样 100 ms", "Wave sampling 100 ms"),
                              g_sample_ms == 100 ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_SAMP_BASE + 3, TR("波形采样 200 ms", "Wave sampling 200 ms"),
                              g_sample_ms == 200 ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_SAMP_BASE + 4, TR("波形采样 500 ms", "Wave sampling 500 ms"),
                              g_sample_ms == 500 ? MF_CHECKED : 0);
               AppendMenuW(m, MF_SEPARATOR, 0, NULL);
               menu_item_utf8(m, IDM_LANG_BASE + 1, TR("中文（简体）", "中文（简体）"),
                              ui_lang == 0 ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_LANG_BASE + 2, TR("English", "English"),
                              ui_lang == 1 ? MF_CHECKED : 0);
               { POINT sp = p; ClientToScreen(hwnd, &sp);
                 int id = popup_menu(m, hwnd, sp);
                 PostMessageW(hwnd, WM_COMMAND, id, 0); }
            }
            else if (pt_in(m3, p))
            {
               HMENU m = new_popup();
               menu_item_utf8(m, IDM_SKIN_BASE + 1, TR("浅色", "Light"), g_th == &TH_LIGHT ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_SKIN_BASE + 2, TR("深色", "Dark"), g_th == &TH_DARK ? MF_CHECKED : 0);
               menu_item_utf8(m, IDM_SKIN_BASE + 3, TR("蓝灰", "Blue-gray"), g_th == &TH_BLUE ? MF_CHECKED : 0);
               { POINT sp = p; ClientToScreen(hwnd, &sp);
                 int id = popup_menu(m, hwnd, sp);
                 PostMessageW(hwnd, WM_COMMAND, id, 0); }
            }
            else if (pt_in(m4, p))
            {
               PostMessageW(hwnd, WM_COMMAND, IDM_ABOUT, 0);
            }
            return 0;
         }

         /* 左侧按钮 */
         {
            RECT b_scan, b_flash, b_mode, b_pause;
            left_buttons(&L.left, &b_scan, &b_flash, &b_mode, &b_pause);
            if (pt_in(b_scan, p)) { do_scan_or_connect(); return 0; }
            if (pt_in(b_flash, p)) { do_flash_xml(); return 0; }
            if (pt_in(b_mode, p)) { do_slave_mode(); return 0; }
            if (pt_in(b_pause, p))
            {
               g_paused = !g_paused;
               log_push(g_paused ? TR("界面已暂停（波形/数值/日志冻结，主站通信继续）",
                                      "UI paused (waveform/values/log frozen, master keeps running)")
                                 : TR("界面已恢复刷新", "UI resumed"));
               InvalidateRect(hwnd, NULL, FALSE);
               return 0;
            }
         }

         /* 左侧 CiA402 驱动控制区 */
         {
            c402_ui_t cu;
            int i;

            c402_left_layout(&L.left, &cu);
            if (pt_in(cu.hdr, p))
            {
               g_c402_open = !g_c402_open;
               InvalidateRect(hwnd, NULL, FALSE);
               return 0;
            }
            if (g_c402_open && cu.has)
            {
               if (cu.multi && pt_in(cu.sel, p))   /* 切换显示的驱动 */
               {
                  int k, nx = 0;
                  for (k = cu.slave + 1; k <= g_snap.slavecount; ++k)
                     if (g_snap.slave[k].c402) { nx = k; break; }
                  if (!nx)
                     for (k = 1; k < cu.slave; ++k)
                        if (g_snap.slave[k].c402) { nx = k; break; }
                  if (nx)
                  {
                     g_c402_sel = nx;
                     InvalidateRect(hwnd, NULL, FALSE);
                  }
                  return 0;
               }
               if (pt_in(cu.state, p)) { c402_info_show(); return 0; }
               if (pt_in(cu.mode, p)) { c402_mode_click(cu.slave); return 0; }
               for (i = 0; i < cu.nbtn; ++i)
               {
                  if (pt_in(cu.btn[i], p))
                  {
                     c402_btn_click(cu.slave, c402_btn_action(i));
                     return 0;
                  }
               }
            }
         }

         /* 消息区"清空"（同时把 ERR 计数归零） */
         {
            RECT clr = log_clear_rect(&L.log);
            if (pt_in(clr, p))
            {
               g_log_count = 0; g_log_head = 0; g_log_scroll = 0;
               g_log_sel_a = g_log_sel_b = -1;
               g_log_sel_ac = g_log_sel_bc = 0;
               g_err_count = 0;
               InvalidateRect(hwnd, NULL, FALSE);
               return 0;
            }
         }

         /* 消息区：按下开始拖选（按字符定位；拖动高亮，Ctrl+C 复制） */
         if (pt_in(log_text_rect(&L.log), p))
         {
            int idx = log_row_at(&L.log, p);
            if (g_editing_idx >= 0) editval_commit(0);
            if (idx >= 0)
            {
               int col = log_col_at_x(&L.log, idx, p.x);
               g_log_sel_a = g_log_sel_b = idx;
               g_log_sel_ac = g_log_sel_bc = col;
               g_log_selecting = 1;
               SetFocus(hwnd);      /* 保证 Ctrl+C 能送到窗口过程 */
               SetCapture(hwnd);
               InvalidateRect(hwnd, &L.log, FALSE);
            }
            return 0;
         }

         /* 正在编辑时点击别处：取消编辑（回车才发送）；
            双击进入编辑后的补发按下不取消，并把焦点还给编辑框 */
         if (g_editing_idx >= 0)
         {
            if (edit_in_dblclk_guard())
               SetFocus(g_edit_val);
            else
               editval_commit(0);
         }

         /* 右侧树 */
         if (pt_in(L.right, p))
         {
            trow_t rows[512];
            int n, k;
            RECT trk, th, trc;
            int need = tree_sb_track(&L.right, &trk);

            /* 滚动条：拖滑块 / 点轨道翻页 */
            if (need)
            {
               th = tree_sb_thumb(&L.right, &need);
               if (pt_in(th, p))
               {
                  g_sb_drag = 1;
                  g_sb_grab = p.y - th.top;
                  SetCapture(hwnd);
                  return 0;
               }
               if (pt_in(trk, p))
               {
                  int vis = tree_vis_rows(&L.right);
                  int total = tree_count_rows();
                  g_tree_scroll += (p.y < th.top) ? -vis : vis;
                  if (g_tree_scroll < 0) g_tree_scroll = 0;
                  if (g_tree_scroll > total - vis) g_tree_scroll = total - vis;
                  InvalidateRect(hwnd, NULL, FALSE);
                  return 0;
               }
            }

            trc = tree_content_rect(&L.right);
            n = tree_build(rows, 512, &trc);
            for (k = 0; k < n; ++k)
            {
               const trow_t *r = &rows[k];
               if (!pt_in(r->rc, p)) continue;
               if (r->kind == RK_SLAVE)
               {
                  g_slave_exp[r->slave] = !g_slave_exp[r->slave];
                  InvalidateRect(hwnd, NULL, FALSE);
               }
               else if (r->kind == RK_SECTION)
               {
                  g_sec_exp[r->slave][r->var] = !g_sec_exp[r->slave][r->var];
                  InvalidateRect(hwnd, NULL, FALSE);
               }
               else if (r->kind == RK_VAR)
               {
                  /* 变量行左键不做波形操作（改由右键添加/移除），
                     输出变量仍可双击数值框进入编辑 */
               }
               break;
            }
            return 0;
         }

         /* 波形面板"清空"：清掉全部波形数据，通道保留继续采样 */
         if (pt_in(wave_clear_rect(&L.wave), p))
         {
            int ci;
            EnterCriticalSection(&g_wave_cs);
            g_tick_total = 0;
            for (ci = 0; ci < g_chan_count; ++ci) g_chan[ci].tick0 = 0;
            g_follow = 1;
            g_right_age = 0.0;
            LeaveCriticalSection(&g_wave_cs);
            log_push(TR("波形已清空", "Waveform cleared"));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
         }

         /* 波形区：开始拖动 */
         {
            RECT pr = wave_plot_rect(&L.wave);
            if (pt_in(pr, p))
            {
               g_dragging = 1;
               g_drag_x = p.x; g_drag_y = p.y;
               SetCapture(hwnd);
            }
            return 0;
         }
      }

      case WM_RBUTTONDOWN:
      {
         POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };

         /* 变量树：右键输入/输出变量行 -> 添加/移除波形通道
            （左键留给数值编辑与常规交互，避免误触） */
         if (pt_in(L.right, p))
         {
            trow_t rows[512];
            RECT trc = tree_content_rect(&L.right);
            int n = tree_build(rows, 512, &trc), k;
            for (k = 0; k < n; ++k)
            {
               const trow_t *r = &rows[k];
               if (!pt_in(r->rc, p)) continue;
               if (r->kind == RK_VAR)
               {
                  chan_toggle(r->var);
                  InvalidateRect(hwnd, NULL, FALSE);
               }
               break;
            }
            return 0;
         }
         break;   /* 其他区域：右键保留系统默认行为 */
      }

      case WM_LBUTTONDBLCLK:
      {
         POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
         RECT pr = wave_plot_rect(&L.wave);
         if (pt_in(pr, p))
         {
            g_follow = 1;
            g_right_age = 0.0;
            g_ymode_auto = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
         }

         /* 变量树：双击输出变量数值 -> 进入就地编辑（回车发送 / ESC 取消） */
         if (pt_in(L.right, p) && g_editing_idx < 0)
         {
            trow_t rows[512];
            RECT trc;
            int n, k;

            trc = tree_content_rect(&L.right);
            n = tree_build(rows, 512, &trc);
            for (k = 0; k < n; ++k)
            {
               const trow_t *r = &rows[k];
               if (r->kind == RK_VAR && pt_in(r->rc, p))
               {
                  const tc3_variable_t *v = &g_snap.vl.var[r->var];
                  if (v->dir == 0)
                     editval_begin(hwnd, &r->val, r->var);
                  break;
               }
            }
         }
         return 0;
      }

      case WM_MOUSEMOVE:
      {
         POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
         int hov_val = -1;

         /* 消息栏拖选：按下并移动时按字符实时扩展高亮区间 */
         if (g_log_selecting)
         {
            int idx = log_row_at(&L.log, p);
            if (idx >= 0)
            {
               int col = log_col_at_x(&L.log, idx, p.x);
               if (idx != g_log_sel_b || col != g_log_sel_bc)
               {
                  g_log_sel_b = idx;
                  g_log_sel_bc = col;
                  InvalidateRect(hwnd, &L.log, FALSE);
               }
            }
            return 0;
         }

         /* 变量树滚动条拖动 */
         if (g_sb_drag)
         {
            RECT trk, th;
            int need = tree_sb_track(&L.right, &trk);
            if (need)
            {
               int span, ty, total, vis, maxs;
               th = tree_sb_thumb(&L.right, &need);
               span = (trk.bottom - trk.top) - (th.bottom - th.top);
               if (span > 0)
               {
                  total = tree_count_rows();
                  vis = tree_vis_rows(&L.right);
                  maxs = total - vis;
                  if (maxs < 1) maxs = 1;
                  ty = p.y - g_sb_grab - trk.top;
                  if (ty < 0) ty = 0;
                  if (ty > span) ty = span;
                  g_tree_scroll = (int)((long long)ty * maxs / span);
                  InvalidateRect(hwnd, NULL, FALSE);
               }
            }
            return 0;
         }

         /* 波形拖动平移 */
         if (g_dragging)
         {
            RECT pr = wave_plot_rect(&L.wave);
            double pw = pr.right - pr.left;
            double dx = p.x - g_drag_x;
            double dy = p.y - g_drag_y;
            double max_age = wave_max_age();

            g_right_age += dx * g_span_ms / pw;
            if (g_right_age < 0.0) g_right_age = 0.0;
            if (g_right_age > max_age) g_right_age = max_age;
            if (g_right_age > 0.5) g_follow = 0;
            if (!g_ymode_auto)
            {
               double ph = pr.bottom - pr.top;
               g_ylo += dy * (g_yhi - g_ylo) / ph;
               g_yhi += dy * (g_yhi - g_ylo) / ph;
            }
            g_drag_x = p.x; g_drag_y = p.y;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
         }

         /* 波形区悬停：记录游标位置（draw_wave 据此画测量游标与数值浮框） */
         if (g_chan_count > 0 && pt_in(wave_plot_rect(&L.wave), p))
         {
            TRACKMOUSEEVENT tme;
            if (!g_wave_hover)
            {
               tme.cbSize = sizeof(tme);
               tme.dwFlags = TME_LEAVE;
               tme.hwndTrack = hwnd;
               tme.dwHoverTime = 0;
               TrackMouseEvent(&tme);
            }
            g_wave_hover = 1;
            g_wave_mx = p.x; g_wave_my = p.y;
         }
         else if (g_wave_hover)
            g_wave_hover = 0;   /* 定时器会整体重绘，无需单独 Invalidate */

         /* 变量树面板内移动：只重绘变量树区域（不整窗失效），
            避免鼠标滑过时波形面板/编辑框等无关区域反复重绘造成闪烁 */
         if (pt_in(L.right, p))
            InvalidateRect(hwnd, &L.right, FALSE);

         /* 输出变量数值框悬停 */
         if (pt_in(L.right, p) && g_editing_idx < 0)
         {
            trow_t rows[512];
            RECT trc = tree_content_rect(&L.right);
            int n = tree_build(rows, 512, &trc), k;
            for (k = 0; k < n; ++k)
               if (rows[k].kind == RK_VAR && rows[k].val.right > rows[k].val.left
                   && pt_in(rows[k].val, p))
               { hov_val = rows[k].var; break; }
         }
         if (hov_val != g_hover_val_var)
         {
            g_hover_val_var = hov_val;
            InvalidateRect(hwnd, &L.right, FALSE);   /* 只重绘变量树区域 */
         }
         return 0;
      }

      case WM_LBUTTONUP:
         /* 消息栏拖选结束：只保留高亮选中，复制交给 Ctrl+C */
         if (g_log_selecting)
         {
            g_log_selecting = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, &L.log, FALSE);
            return 0;
         }
         if (g_dragging)
         {
            g_dragging = 0;
            ReleaseCapture();
         }
         if (g_sb_drag)
         {
            g_sb_drag = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
         }
         return 0;

      case WM_SETCURSOR:   /* 消息文字区显示 I 型光标，提示可拖选复制 */
         if (LOWORD(lp) == HTCLIENT)
         {
            POINT cp;
            RECT lr, clrb;
            GetCursorPos(&cp);
            ScreenToClient(hwnd, &cp);
            lr = log_text_rect(&L.log);
            clrb = log_clear_rect(&L.log);
            if (pt_in(lr, cp) && !pt_in(clrb, cp))
            {
               SetCursor(LoadCursorW(NULL, IDC_IBEAM));
               return TRUE;
            }
         }
         break;

      case WM_KEYDOWN:
         if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
         {
            if (wp == 'C' || wp == 'c')          /* Ctrl+C：复制当前选择（无提示） */
            {
               log_copy_sel();
               return 0;
            }
            if (wp == 'A' || wp == 'a')          /* Ctrl+A：全选消息 */
            {
               if (g_log_count > 0)
               {
                  g_log_sel_a = 0;
                  g_log_sel_ac = 0;
                  g_log_sel_b = g_log_count - 1;
                  g_log_sel_bc = 0x7FFFFFFF;     /* 行尾（绘制/复制时会钳制到实际长度） */
                  InvalidateRect(hwnd, &L.log, FALSE);
               }
               return 0;
            }
         }
         if (wp == VK_ESCAPE && g_log_sel_a >= 0)  /* ESC：取消选择 */
         {
            g_log_sel_a = g_log_sel_b = -1;
            g_log_sel_ac = g_log_sel_bc = 0;
            InvalidateRect(hwnd, &L.log, FALSE);
            return 0;
         }
         break;

      case WM_MOUSELEAVE:   /* 鼠标离开窗口：清除波形测量游标 */
         if (g_wave_hover)
         {
            g_wave_hover = 0;
            InvalidateRect(hwnd, NULL, FALSE);
         }
         return 0;

      case WM_MOUSEWHEEL:
      {
         POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
         int delta = GET_WHEEL_DELTA_WPARAM(wp);
         ScreenToClient(hwnd, &p);

         if (pt_in(L.wave, p) && g_chan_count > 0)
         {
            RECT pr = wave_plot_rect(&L.wave);
            if (wp & MK_CONTROL)   /* Y 轴缩放 */
            {
               double f = (delta > 0) ? 1.0 / 1.2 : 1.2;
               double v = g_ylo + (pr.bottom - p.y) /
                          (double)(pr.bottom - pr.top) * (g_yhi - g_ylo);
               if (!g_ymode_auto)
               {
                  g_ylo = v - (v - g_ylo) * f;
                  g_yhi = v + (g_yhi - v) * f;
               }
               else
               {
                  double lo, hi;
                  wave_auto_y(&pr, &lo, &hi);
                  g_ymode_auto = 0;
                  g_ylo = v - (v - lo) * f;
                  g_yhi = v + (hi - v) * f;
               }
            }
            else   /* X 轴（时间）缩放 */
            {
               double f = (delta > 0) ? 1.0 / 1.2 : 1.2;
               double span = g_span_ms * f;
               double max_age = wave_max_age();
               if (span < g_sample_ms * 2.0) span = g_sample_ms * 2.0;
               if (span > max_age + 60000.0 && f > 1.0) span = max_age + 60000.0;
               if (!g_follow)
               {
                  double age_c = x_to_age(p.x, &pr);
                  g_right_age = age_c - (pr.right - p.x) /
                                (double)(pr.right - pr.left) * span;
                  if (g_right_age < 0.0) g_right_age = 0.0;
                  if (g_right_age > max_age) g_right_age = max_age;
                  if (g_right_age <= 0.5) g_follow = 1;
               }
               g_span_ms = span;
            }
            InvalidateRect(hwnd, NULL, FALSE);
         }
         else if (pt_in(L.right, p))   /* 变量树滚动 */
         {
            g_tree_scroll -= (delta / WHEEL_DELTA) * 3;
            if (g_tree_scroll < 0) g_tree_scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
         }
         else if (pt_in(L.log, p))     /* 消息回看 */
         {
            g_log_scroll += (delta / WHEEL_DELTA) * 3;
            if (g_log_scroll < 0) g_log_scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
         }
         return 0;
      }

      case WM_CTLCOLOREDIT:   /* 数值编辑框：高亮底色 + 强调色文字（无矩形边框） */
         if ((HWND)lp == g_edit_val)
         {
            static HBRUSH br = NULL;
            static COLORREF br_c = 0;
            HDC hdc = (HDC)wp;
            if (br == NULL || br_c != g_th->sel)
            {
               if (br) DeleteObject(br);
               br = CreateSolidBrush(g_th->sel);
               br_c = g_th->sel;
            }
            SetBkColor(hdc, g_th->sel);
            SetTextColor(hdc, g_th->accent);
            return (LRESULT)br;
         }
         if ((HWND)lp == g_edit_cap)   /* 缓冲区上限编辑框：面板底色 + 常规文字 */
         {
            static HBRUSH br2 = NULL;
            static COLORREF br2_c = 0;
            HDC hdc = (HDC)wp;
            if (br2 == NULL || br2_c != g_th->btnface)
            {
               if (br2) DeleteObject(br2);
               br2 = CreateSolidBrush(g_th->btnface);
               br2_c = g_th->btnface;
            }
            SetBkColor(hdc, g_th->btnface);
            SetTextColor(hdc, g_th->text);
            return (LRESULT)br2;
         }
         break;

      case WM_COMMAND:
      {
         int id = LOWORD(wp);
         int note = HIWORD(wp);

         if (id == 95 && note == EN_KILLFOCUS)   /* 数值编辑框失焦 -> 取消（回车才发送） */
         {
            if (!edit_in_dblclk_guard())         /* 双击补发按下引发的瞬时失焦不算 */
               editval_commit(0);
            return 0;
         }
         switch (id)
         {
            case IDM_NIC:
               do_select_adapter();
               break;
            case IDM_FLASH:
               do_flash_xml();
               break;
            case IDM_OPXMLDIR:
               ShellExecuteW(hwnd, L"open", L"xml", NULL, NULL, SW_SHOWNORMAL);
               break;
            case IDM_EXIT:
               DestroyWindow(hwnd);
               break;
            case IDM_DC:
               apply_slave_mode(1);
               break;
            case IDM_FREERUN:
               apply_slave_mode(0);
               break;
            case IDM_SAMP_BASE + 1:
            case IDM_SAMP_BASE + 2:
            case IDM_SAMP_BASE + 3:
            case IDM_SAMP_BASE + 4:
            case IDM_SAMP_BASE + 5:   /* 10 ms */
            case IDM_SAMP_BASE + 6:   /* 5 ms */
            case IDM_SAMP_BASE + 7:   /* 1 ms */
            {
               static const int ms_tab[8] = { 0, 50, 100, 200, 500, 10, 5, 1 };
               int ms = ms_tab[id - IDM_SAMP_BASE];
               g_sample_ms = ms;
               config_save_now();      /* 采样周期也记住 */
               break;
            }
            case IDM_SKIN_BASE + 1: g_th = &TH_LIGHT; config_save_now(); break;
            case IDM_SKIN_BASE + 2: g_th = &TH_DARK;  config_save_now(); break;
            case IDM_SKIN_BASE + 3: g_th = &TH_BLUE;  config_save_now(); break;
            case IDM_LANG_BASE + 1:            /* 中文 */
               if (ui_lang != 0)
               {
                  ui_lang = 0;
                  g_cfg.lang = 0;
                  config_save_now();
                  log_push(TR("界面语言已切换为中文", "UI language switched to Chinese"));
               }
               break;
            case IDM_LANG_BASE + 2:            /* English */
               if (ui_lang != 1)
               {
                  ui_lang = 1;
                  g_cfg.lang = 1;
                  config_save_now();
                  log_push(TR("界面语言已切换为英文", "UI language switched to English"));
               }
               break;
            case IDM_ABOUT:
               msgbox_utf8(
                  TR("SimpleCAT V1.0\n\n"
                     "轻量级 EtherCAT 主站（基于 SOEM）\n\n"
                     "功能：总线扫描 / PDO 监视与写入 / DC 与 FreeRUN /\n"
                     "EEPROM 烧录（ESI XML）/ 多变量实时波形 /\n"
                     "CiA402 伺服驱动控制（一键使能/故障复位/运行模式）\n\n"
                     "波形操作：滚轮缩放时间轴，Ctrl+滚轮缩放 Y 轴，\n"
                     "拖动平移，双击恢复跟随最新。\n\n"
                     "CiA402：连接总线后，右侧从站树中展开\n"
                     "「CiA402 驱动控制」即可控制伺服驱动器。\n\n"
                     "作者：坚果侠一号",
                     "SimpleCAT V1.0\n\n"
                     "Lightweight EtherCAT master (based on SOEM)\n\n"
                     "Features: bus scan / PDO monitoring & writing / DC and FreeRUN /\n"
                     "EEPROM flashing (ESI XML) / multi-variable live waveform /\n"
                     "CiA402 servo drive control (auto enable/fault reset/mode)\n\n"
                     "Waveform: mouse wheel zooms the time axis, Ctrl+wheel zooms Y,\n"
                     "drag to pan, double-click to restore live follow.\n\n"
                     "CiA402: after connecting, expand \"CiA402 Drive Control\"\n"
                     "in the right slave tree to control servo drives.\n\n"
                     "Author: 坚果侠一号"),
                  TR("软件信息", "About"), MB_OK | MB_ICONINFORMATION);
               break;
            default:
               break;
         }
         InvalidateRect(hwnd, NULL, FALSE);
         return 0;
      }

      case WM_GETMINMAXINFO:
      {
         MINMAXINFO *mmi = (MINMAXINFO *)lp;
         mmi->ptMinTrackSize.x = MIN_W;
         mmi->ptMinTrackSize.y = MIN_H;
         return 0;
      }

      case WM_DESTROY:
         config_save_now();   /* 退出时把窗口位置/皮肤/模式等写入配置 */
         KillTimer(hwnd, 1);
         ui_master_stop();
         PostQuitMessage(0);
         return 0;
   }
   return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================ 入口 ============================ */

int ui_app_run(void)
{
   WNDCLASSEXW wc;
   HWND hwnd;
   MSG msg;
   HINSTANCE hi = GetModuleHandleW(NULL);
   int cfg_loaded;
   int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = WIN_W, wh = WIN_H;

   /* 图形界面不需要控制台窗口（若作为控制台程序启动则释放） */
   FreeConsole();

   /* 读取上次保存的界面配置（皮肤/从站模式/采样周期/窗口几何） */
   cfg_loaded = ui_config_load(&g_cfg);
   g_th = theme_from_id(g_cfg.skin);
   g_sample_ms = g_cfg.sample_ms;
   g_wave_cap = g_cfg.wave_buf;      /* 每通道缓冲区上限（ini 校验过范围） */
   ui_lang = (g_cfg.lang == 1) ? 1 : 0;   /* 界面语言（TR 宏） */
   ui_master_set_mode_pref(g_cfg.slave_mode);   /* 启动序列据此决定 DC/FreeRUN */

   InitializeCriticalSection(&g_wave_cs);
   wave_bufs_alloc();                /* 为全部通道槽位分配环形缓冲 */
   ui_master_init();

   /* 波形采样线程（1ms 精度，独立于 20ms 界面定时器） */
   g_sample_quit = 0;
   g_sample_thread = CreateThread(NULL, 0, wave_sampler, NULL, 0, NULL);

   memset(&wc, 0, sizeof(wc));
   wc.cbSize = sizeof(wc);
   wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
   wc.lpfnWndProc = wnd_proc;
   wc.hInstance = hi;
   wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
   wc.hbrBackground = NULL;
   wc.lpszClassName = APP_CLASS;
   wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
   RegisterClassExW(&wc);

   /* 有保存的几何且仍落在某个显示器范围内才沿用，否则用系统默认位置 */
   if (g_cfg.win_w >= MIN_W && g_cfg.win_h >= MIN_H)
   {
      RECT r;
      r.left = g_cfg.win_x; r.top = g_cfg.win_y;
      r.right = r.left + g_cfg.win_w;
      r.bottom = r.top + g_cfg.win_h;
      if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != NULL)
      {
         wx = g_cfg.win_x;
         wy = g_cfg.win_y;
         ww = g_cfg.win_w;
         wh = g_cfg.win_h;
      }
   }

   hwnd = CreateWindowExW(0, APP_CLASS, APP_TITLE,
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                          wx, wy, ww, wh,
                          NULL, NULL, hi, NULL);
   (void)hwnd;

   if (g_cfg.win_max)
      ShowWindow(hwnd, SW_SHOWMAXIMIZED);
   if (cfg_loaded)
      log_push(TR("已载入上次保存的界面设置（皮肤/从站模式/窗口位置）",
                  "Loaded saved UI settings (skin/slave mode/window position)"));

   while (GetMessageW(&msg, NULL, 0, 0) > 0)
   {
      if (IsDialogMessageW(hwnd, &msg)) /* 让编辑框 Tab/回车可用 */
         continue;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }

   /* 停止波形采样线程 */
   InterlockedExchange(&g_sample_quit, 1);
   if (g_sample_thread)
   {
      WaitForSingleObject(g_sample_thread, 2000);
      CloseHandle(g_sample_thread);
      g_sample_thread = NULL;
   }
   DeleteCriticalSection(&g_wave_cs);
   return (int)msg.wParam;
}

