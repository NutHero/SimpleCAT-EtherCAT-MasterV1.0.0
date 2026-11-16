/** \file
 * \brief Example code for Simple Open EtherCAT master（控制台版）
 *
 * 本文件是 SimpleCAT 的控制台交互入口（--console 或直接传入网卡名启动）。
 * 主站核心逻辑已抽取到 src/master_core.c（与图形界面版共用）；
 * 无参数启动时进入图形界面版（见 ui/ui_app.c）。
 *
 * Usage: soem_io.exe --console            （自动枚举网卡，控制台交互）
 *        soem_io.exe --ui                 （图形界面）
 *        soem_io.exe \Device\NPF_{GUID}   （控制台版 + 指定网卡）
 * IFNAME1 is the NIC interface name, e.g. 'eth0'
 */

#include "master_core.h"
#include "esi_flash.h"
#include "ui_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* 启用控制台 VT 转义序列 + UTF-8 输出（用于原地刷新界面） */
static void setup_console(void)
{
   HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
   DWORD mode = 0;

   SetConsoleOutputCP(CP_UTF8);
   SetConsoleCP(CP_UTF8);
   if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
   {
      mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      SetConsoleMode(hOut, mode);
   }
}

/**
 * 自动枚举并选择网络适配器（网卡）
 * 使用 master_list_adapters() 枚举系统上所有可用的网卡接口：
 *  - 只找到 1 个网卡：自动选用，无需人工干预
 *  - 找到多个网卡：打印列表，由用户输入序号选择
 *  - 未找到网卡：返回失败
 *
 * @param[out] ifbuf   用于存放选中接口名称的缓冲区
 * @param[in]  bufsize 缓冲区大小
 * @return 成功选中接口返回 TRUE，否则返回 FALSE
 */
static boolean
auto_select_adapter(char *ifbuf, size_t bufsize)
{
   ec_adaptert *head;
   ec_adaptert *candidates[32]; /* 最多列出 32 个候选网卡 */
   int count = 0;
   int choice = 1;
   int i;

   head = master_list_adapters(&count);
   {
      ec_adaptert *adapter = head;
      int k = 0;
      while (adapter != NULL)
      {
         if (k < (int)(sizeof(candidates) / sizeof(candidates[0])))
         {
            candidates[k] = adapter;
         }
         k++;
         adapter = adapter->next;
      }
   }

   if (count == 0)
   {
      printf("No network adapter found!\n");
      printf("Please check that WinPcap/Npcap is installed and the NIC is enabled.\n");
      ec_free_adapters(head);
      return FALSE;
   }

   /* 打印所有可用网卡 */
   printf("\nAvailable adapters:\n");
   for (i = 0; i < count && i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i)
   {
      printf("  [%d] %s  (%s)\n", i + 1, candidates[i]->name, candidates[i]->desc);
   }
   if (count > (int)(sizeof(candidates) / sizeof(candidates[0])))
   {
      printf("  ... (%d adapters total, showing first %d)\n",
             count, (int)(sizeof(candidates) / sizeof(candidates[0])));
   }

   if (count == 1)
   {
      /* 只有一个网卡，自动选用 */
      choice = 1;
      printf("Auto-selected adapter [1]\n");
   }
   else
   {
      /* 多个网卡，让用户选择 */
      printf("Select adapter [1-%d] (default 1): ", count);
      if (scanf("%d", &choice) != 1 || choice < 1 || choice > count)
      {
         choice = 1; /* 非法输入时回退到第一个 */
      }
   }

   strncpy(ifbuf, candidates[choice - 1]->name, bufsize - 1);
   ifbuf[bufsize - 1] = '\0';
   printf("Using adapter: %s\n", ifbuf);

   ec_free_adapters(head);
   return TRUE;
}

/* ==================== 实时监控 + 交互下发 ==================== */

#define LIVE_REFRESH_MS   200    /* 实时显示刷新周期 */
#define LIVE_CYCLE_US     1000   /* 过程数据交换周期（1ms） */
#define EDIT_BUF_SIZE     128

/* 控制台颜色（VT 转义序列） */
#define CLR_RESET "\x1b[0m"
#define CLR_RED   "\x1b[31m"
#define CLR_GRN   "\x1b[32m"
#define CLR_YEL   "\x1b[33m"
#define CLR_CYN   "\x1b[36m"

static int g_quit = 0;      /* 收到 quit 命令 */
static int g_showlist = 0;  /* 收到 list 命令，重打印扫描表 */

/* ESI 匹配警告（xml/ 与从站身份不符时非空，实时界面显示） */
static char g_esi_warn[192] = "";

/* ---------------- EEPROM 烧录（flash 命令）对话框状态机 ---------------- */

typedef enum
{
   FL_NONE = 0,    /* 未在烧录流程中 */
   FL_SELECT,      /* 等待选择 xml 文件序号 */
   FL_CONFIRM,     /* 等待确认 y/n */
   FL_MISMATCH,    /* XML 身份与从站不符，等待用户决定是否强制烧录 */
   FL_BURN         /* 正在构建镜像 + 烧录 */
} flash_state_t;

static flash_state_t g_flash_state = FL_NONE;
static int    g_flash_shown = 0;      /* 当前对话框提示是否已打印 */
static char   g_flash_files[ESI_FLASH_MAX_FILES][ESI_FLASH_NAME_MAX];
static int    g_flash_nfiles = 0;
static int    g_flash_pick = -1;      /* 选中的文件下标 */
static int    g_flash_force = 0;      /* 1 = 身份不符仍强制烧录（用户已确认） */
static int    g_flash_idwords = 0;    /* 身份区字数（0 = 不烧身份区） */
static char   g_flash_mismatch[200] = ""; /* 身份不符的详情（FL_MISMATCH 显示） */
static uint16 g_flash_img[ESI_FLASH_MAX_WORDS];
static int    g_flash_start = 0;      /* 起始字地址（0x0040，固定区不参与） */
static int    g_flash_words = 0;      /* 类别区字数 */

/* 烧录进度回调：绿色加载条（写 + 校验共 2*words 步） */
static void flash_progress_cb(void *user, int done, int total)
{
   int pct = (total > 0) ? (int)((long long)done * 100 / total) : 0;
   int fill = pct / 5; /* 20 格 */
   int i;

   (void)user;
   if (fill > 20)
      fill = 20;
   printf("\x1b[32m\r  [");
   for (i = 0; i < fill; i++)
      printf("█");
   for (; i < 20; i++)
      printf("░");
   printf("] %3d%%  (%d/%d 字)\x1b[0m", pct, done, total);
   fflush(stdout);
}

static void flash_cancel(char *msg, size_t msz)
{
   g_flash_state = FL_NONE;
   g_flash_shown = 0;
   g_flash_pick = -1;
   g_flash_force = 0;
   g_flash_idwords = 0;
   g_flash_mismatch[0] = '\0';
   snprintf(msg, msz, "已取消烧录");
}

/* FL_SELECT 状态下用户输入的文件序号 */
static void flash_pick_file(const char *cmd, char *msg, size_t msz)
{
   int n;

   if (cmd[0] == 'c' || cmd[0] == 'C')
   {
      flash_cancel(msg, msz);
      return;
   }
   n = atoi(cmd);
   if (n < 1 || n > g_flash_nfiles)
   {
      snprintf(msg, msz, "无效的文件序号 %s（1-%d，c 取消）",
               cmd, g_flash_nfiles > 0 ? g_flash_nfiles : 0);
      return;
   }
   g_flash_pick = n - 1;
   g_flash_state = FL_CONFIRM;
   g_flash_shown = 0;
   snprintf(msg, msz, "已选择 %s", g_flash_files[g_flash_pick]);
}

/* FL_CONFIRM 状态下用户输入的确认 */
static void flash_confirm_burn(const char *cmd, char *msg, size_t msz)
{
   if (_stricmp(cmd, "y") == 0 || _stricmp(cmd, "yes") == 0)
   {
      g_flash_state = FL_BURN;
      g_flash_shown = 0;
      snprintf(msg, msz, "正在准备烧录 ...");
   }
   else if (_stricmp(cmd, "n") == 0 || _stricmp(cmd, "no") == 0 ||
            cmd[0] == 'c' || cmd[0] == 'C')
   {
      flash_cancel(msg, msz);
   }
   else
   {
      snprintf(msg, msz, "请输入 y 确认烧录，或 n 取消");
   }
}

/*
 * 处理用户命令：
 *   变量名=值        如 M1_Speed=-360 （写入输出 PDO 变量）
 *   0x7010:03=100    按索引:子索引写入
 *   list             重新打印完整扫描表
 *   quit / q         退出
 */
static void process_command(char *cmd, const tc3_varlist_t *vl, ec_groupt *grp,
                            char *last_msg, size_t lmsz)
{
   char name[64] = "";
   char val[64] = "";
   char *eq;
   int idx;
   const tc3_variable_t *v;
   long value;

   trim_inplace(cmd);
   if (cmd[0] == '\0')
      return;

   if (_stricmp(cmd, "quit") == 0 || _stricmp(cmd, "exit") == 0 || _stricmp(cmd, "q") == 0)
   {
      g_quit = 1;
      snprintf(last_msg, lmsz, "正在停止主站...");
      return;
   }
   if (_stricmp(cmd, "list") == 0)
   {
      g_showlist = 1;
      snprintf(last_msg, lmsz, "重新打印扫描表...");
      return;
   }

   if (_stricmp(cmd, "flash") == 0)
   {
      if (g_flash_state != FL_NONE)
      {
         snprintf(last_msg, lmsz, "烧录流程已在进行中");
      }
      else
      {
         g_flash_force = 0;
         g_flash_idwords = 0;
         g_flash_state = FL_SELECT;
         g_flash_shown = 0;
         snprintf(last_msg, lmsz, "进入 EEPROM 烧录模式");
      }
      return;
   }

   /* 烧录对话框激活期间，输入行由对话框状态机接管 */
   if (g_flash_state == FL_SELECT)
   {
      flash_pick_file(cmd, last_msg, lmsz);
      return;
   }
   if (g_flash_state == FL_CONFIRM)
   {
      flash_confirm_burn(cmd, last_msg, lmsz);
      return;
   }
   if (g_flash_state == FL_MISMATCH)
   {
      /* 身份不符后的二次确认：y = 强制烧录（换装新 ESI） */
      if (_stricmp(cmd, "y") == 0 || _stricmp(cmd, "yes") == 0)
      {
         g_flash_force = 1;
         g_flash_state = FL_BURN;
         g_flash_shown = 0;
         snprintf(last_msg, lmsz, "已确认强制烧录 ...");
      }
      else if (_stricmp(cmd, "n") == 0 || _stricmp(cmd, "no") == 0 ||
               cmd[0] == 'c' || cmd[0] == 'C')
      {
         flash_cancel(last_msg, lmsz);
      }
      else
      {
         snprintf(last_msg, lmsz, "身份不符：输入 y 强制烧录，n 取消");
      }
      return;
   }

   eq = strchr(cmd, '=');
   if (eq == NULL)
   {
      snprintf(last_msg, lmsz, "格式错误，示例: M1_Speed=-360 或 0x7010:03=100");
      return;
   }
   *eq = '\0';
   snprintf(name, sizeof(name), "%s", cmd);
   snprintf(val, sizeof(val), "%s", eq + 1);
   trim_inplace(name);
   trim_inplace(val);
   if (name[0] == '\0' || val[0] == '\0')
   {
      snprintf(last_msg, lmsz, "格式错误，示例: M1_Speed=-360");
      return;
   }
   value = strtol(val, NULL, 0); /* 支持十进制 / 0x 十六进制 / 负数 */

   /* 按名称或索引查找输出变量；索引格式如 0x7010:03 */
   if ((name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) || name[0] == '#')
   {
      uint16 ix;
      uint8 sb;
      char *colon = strchr(name, ':');
      char *hx;

      if (colon == NULL)
      {
         snprintf(last_msg, lmsz, "索引格式应为 0x7010:03");
         return;
      }
      *colon = '\0';
      hx = (name[0] == '#') ? name + 2 : name;
      ix = (uint16)strtoul(hx, NULL, 16);
      sb = (uint8)strtoul(colon + 1, NULL, 16);
      idx = find_var_by_index(vl, ix, sb, 0);
      if (idx < 0 && find_var_by_index(vl, ix, sb, 1) >= 0)
      {
         snprintf(last_msg, lmsz, "%s 是输入(TxPDO)变量，只读", name);
         return;
      }
   }
   else
   {
      idx = find_var_by_name(vl, name, 0);
      if (idx < 0 && find_var_by_name(vl, name, 1) >= 0)
      {
         snprintf(last_msg, lmsz, "%s 是输入(TxPDO)变量，只读", name);
         return;
      }
   }

   if (idx < 0)
   {
      snprintf(last_msg, lmsz, "未找到输出变量 %s（输入 list 查看变量表）", name);
      return;
   }

   v = &vl->var[idx];
   if (grp->outputs == NULL)
   {
      snprintf(last_msg, lmsz, "输出缓冲未就绪");
      return;
   }
   write_var_value(v, grp->outputs, value, last_msg, lmsz);
}

/*
 * 原地刷新实时界面（利用 VT 转义序列把光标移回表格顶部重画，
 * 避免滚屏。刷新期间若用户正在输入则跳过，防止覆盖编辑行）
 */
static void live_view(Fieldbus *fieldbus, const tc3_varlist_t *vl, int *prev_lines,
                      const char *last_msg, const char *editbuf,
                      int wkc, int expected, int rt_us)
{
   ec_groupt *grp = fieldbus->context.grouplist + fieldbus->group;
   int i;
   int n = 0;
   int nslaves = fieldbus->context.slavecount;
   int nop = 0;
   int bus_ok = (wkc >= expected);

   for (i = 1; i <= nslaves; ++i)
   {
      if (fieldbus->context.slavelist[i].state == EC_STATE_OPERATIONAL)
         nop++;
   }

   /* 上一次绘制结束时，光标停在本帧最后一行的行尾（该行无换行符），
      所以回退 *prev_lines - 1 行即可精确回到本帧首行。
      （若回退 *prev_lines 行会多退一行，导致界面每次刷新向上漂移滚动）
      注意：CSI nA 只移动行不动列，必须跟一个 \r 把列归零，
      否则首行会残留上一次的旧字符，出现内容错位。 */
   if (*prev_lines > 1)
      printf("\x1b[%dA\r", *prev_lines - 1);

   printf("\x1b[K%s========== 实时过程数据   WKC %d/%d   往返 %d us   从站 OP %d/%d ==========%s\n",
          bus_ok ? CLR_GRN : CLR_RED, wkc, expected, rt_us, nop, nslaves, CLR_RESET);
   n++;

   /* xml/ 目录与从站身份不匹配的醒目警告（换错 XML / 烧录后未重新上电） */
   if (g_esi_warn[0])
   {
      printf("\x1b[K  " CLR_YEL "⚠ %s" CLR_RESET "\n", g_esi_warn);
      n++;
   }

   /* ---------- 从站连接状态（TwinCAT3 风格） ---------- */
   printf("\x1b[K  " CLR_CYN "从站状态" CLR_RESET ":\n");
   n++;
   for (i = 1; i <= nslaves; ++i)
   {
      ec_slavet *sl = &fieldbus->context.slavelist[i];
      char stbuf[24];
      char link[8];
      const char *clr;
      int p, lp = 0;

      slave_state_label(sl->state, stbuf, sizeof(stbuf));

      if (sl->islost)
         clr = CLR_RED;
      else if (sl->state == EC_STATE_OPERATIONAL)
         clr = CLR_GRN;
      else if (sl->state == EC_STATE_SAFE_OP || sl->state == EC_STATE_PRE_OP)
         clr = CLR_YEL;
      else
         clr = CLR_RED;

      for (p = 0; p < 4; ++p) /* DL status bit0..3 = 端口 0..3 链路 */
      {
         if (master_linkstat(i) & (1u << p))
            link[lp++] = (char)('A' + p);
      }
      if (lp == 0)
         link[lp++] = '-';
      link[lp] = '\0';

      printf("\x1b[K    #%d %-20s %s[%-9s]" CLR_RESET
             " AL 0x%04X  %-4s Link %s",
             i, sl->name, clr, stbuf, sl->state,
             sl->islost ? "LOST" : "UP", link);
      if (sl->ALstatuscode != 0) /* 从站报的应用层错误码（如 0x001E 输入配置无效） */
         printf("  " CLR_YEL "Err 0x%04X %s" CLR_RESET,
                sl->ALstatuscode, ec_ALstatuscode2string(sl->ALstatuscode));
      printf("\n");
      n++;
   }

   printf("\x1b[K  " CLR_CYN "输出" CLR_RESET " (RxPDO -> 从站):\n");
   n++;
   for (i = 0; i < vl->count; i++)
   {
      const tc3_variable_t *v = &vl->var[i];
      if (v->dir != 0)
         continue;
      if (grp->outputs == NULL)
      {
         printf("\x1b[K    %-12s = -\n", v->name);
      }
      else
      {
         printf("\x1b[K    %-12s = %-10lld (0x%04X:%02X %2dbit @0x%04X.%d)\n",
                v->name, read_var_value(v, grp->outputs),
                v->index, v->sub, v->bitlen, v->offset, v->bitoff);
      }
      n++;
   }
   printf("\x1b[K  输入 (TxPDO <- 从站):\n");
   n++;
   for (i = 0; i < vl->count; i++)
   {
      const tc3_variable_t *v = &vl->var[i];
      if (v->dir != 1)
         continue;
      if (grp->inputs == NULL)
      {
         printf("\x1b[K    %-12s = -\n", v->name);
      }
      else
      {
         printf("\x1b[K    %-12s = %-10lld (0x%04X:%02X %2dbit @0x%04X.%d)\n",
                v->name, read_var_value(v, grp->inputs),
                v->index, v->sub, v->bitlen, v->offset, v->bitoff);
      }
      n++;
   }
   printf("\x1b[K  消息: %s\n", last_msg);
   n++;
   printf("\x1b[K  命令(变量名=值 / list / quit) > %s", editbuf);
   printf("\x1b[J"); /* 清除下方残留：上一帧更长（如消息变短）时不会留下旧行 */
   fflush(stdout);
   n++;
   *prev_lines = n;
}

/* ---------------- 启动失败恢复模式 ---------------- */

/*
 * 从站未能进入 OP（如 AL 报 Invalid input configuration，PDO 映射与
 * ESI 不一致、EEPROM 身份/描述与固件不符等）时，不再直接退出程序，
 * 而是进入阻塞式恢复控制台：
 *   flash   烧录 xml/ 下的 ESI 到从站 EEPROM（修身份/描述不匹配）
 *   scan    重新打印扫描表（核对 SyncManager 长度与 ESI PDO 定义）
 *   retry   重新扫描并尝试启动
 *   quit    退出
 * 返回 1 = retry 成功（继续进入实时监控）；0 = 用户选择退出。
 */

/* 恢复模式下的烧录（阻塞式 fgets 输入，复用进度条与身份不符确认） */
static void startup_flash_burn(Fieldbus *fieldbus)
{
   char files[ESI_FLASH_MAX_FILES][ESI_FLASH_NAME_MAX];
   char fpath[300], ferr[200], line[64];
   uint16 img[ESI_FLASH_MAX_WORDS];
   int n, pick, i, r, force;
   int start = 0, words = 0, idwords = 0;

   if (fieldbus->context.slavecount < 1)
   {
      printf("  总线上没有从站，无法烧录\n");
      return;
   }

   n = esi_flash_list_xml("xml", files, ESI_FLASH_MAX_FILES);
   if (n == 0)
   {
      printf("  xml 目录下没有可解析的 .xml 设备描述文件\n");
      return;
   }
   printf("\n  可烧录的 ESI 文件：\n");
   for (i = 0; i < n; i++)
      printf("  [%d] %s\n", i + 1, files[i]);
   printf("  输入文件序号（1-%d），直接回车取消: ", n);
   fflush(stdout);
   if (fgets(line, sizeof(line), stdin) == NULL)
      return;
   trim_inplace(line);
   if (line[0] == '\0')
      return;
   pick = atoi(line) - 1;
   if (pick < 0 || pick >= n)
   {
      printf("  无效序号，已取消\n");
      return;
   }

   snprintf(fpath, sizeof(fpath), "xml/%s", files[pick]);

   printf("  [1/3] 从站 1 切 INIT 并取得 EEPROM 访问权 ...\n");
   fflush(stdout);
   r = esi_flash_begin(&fieldbus->context, 1, ferr, sizeof(ferr));
   if (r != 0)
   {
      printf("  \x1b[31m烧录失败: %s\x1b[0m\n", ferr);
      return;
   }

   force = 0;
   for (;;)
   {
      printf("  [2/3] 构建 SII 镜像（XML → 身份区 + 类别区）...\n");
      fflush(stdout);
      r = esi_flash_build_image(&fieldbus->context, 1, fpath, img,
                                ESI_FLASH_MAX_WORDS, force,
                                &start, &words, &idwords,
                                ferr, sizeof(ferr));
      if (r != ESI_FLASH_ERR_MISMATCH)
         break;
      printf("  \x1b[33m[身份不匹配]\x1b[0m %s\n", ferr);
      printf("  换装新 ESI 时身份本来就不会一致。强制继续? (y/n): ");
      fflush(stdout);
      if (fgets(line, sizeof(line), stdin) == NULL)
         break;
      trim_inplace(line);
      if (line[0] == 'y' || line[0] == 'Y')
      {
         force = 1;
         continue;
      }
      esi_flash_end(&fieldbus->context, 1);
      printf("  已取消烧录\n");
      return;
   }
   if (r != 0)
   {
      esi_flash_end(&fieldbus->context, 1);
      printf("  \x1b[31m烧录失败: %s\x1b[0m\n", ferr);
      return;
   }
   printf("        OK：%d 字身份区 + %d 字类别区\n", idwords, words);

   printf("  [3/3] 写入并校验 ...\n");
   fflush(stdout);
   r = esi_flash_burn(&fieldbus->context, 1, img, start, words,
                      idwords > 0, flash_progress_cb, NULL, ferr, sizeof(ferr));
   printf("\n");
   esi_flash_end(&fieldbus->context, 1);

   if (r >= 0)
   {
      printf("  \x1b[32m烧录完成：共写入并校验 %d 字（含身份区）。\x1b[0m\n", r);
      printf("  请给从站重新上电，然后输入 retry 重新启动主站。\n");
   }
   else
   {
      printf("  \x1b[31m烧录失败: %s\x1b[0m\n", ferr);
   }
}

static int startup_recover(Fieldbus *fieldbus)
{
   char line[128];

   printf("\n==============================================================================\n");
   printf(" \x1b[33m从站未能进入 OP，主站进入恢复模式（不会自动退出）。\x1b[0m\n");
   printf(" 常见原因：xml/ 中的 ESI 描述与从站实际固件不一致 ——\n");
   printf("   · PDO 映射不符：对比扫描表中 SyncManager 长度与 ESI 的 PDO 定义\n");
   printf("     （如 AL 报 Invalid input configuration / Invalid output configuration）\n");
   printf("   · 身份/描述不匹配：EEPROM 中烧录的 ESI 与从站固件不是同一版本\n");
   printf(" 可用命令：\n");
   printf("   flash   烧录 xml/ 下的 ESI 到从站 EEPROM（烧后给从站重新上电）\n");
   printf("   scan    重新打印扫描表\n");
   printf("   retry   重新扫描并尝试启动\n");
   printf("   quit    退出程序\n");
   fflush(stdout);

   for (;;)
   {
      printf("\n(recover) 输入命令 flash / scan / retry / quit > ");
      fflush(stdout);
      if (fgets(line, sizeof(line), stdin) == NULL)
         return 0;
      trim_inplace(line);

      if (_stricmp(line, "quit") == 0 || _stricmp(line, "exit") == 0 ||
          _stricmp(line, "q") == 0)
      {
         return 0;
      }
      if (_stricmp(line, "flash") == 0)
      {
         startup_flash_burn(fieldbus);
         continue;
      }
      if (_stricmp(line, "scan") == 0)
      {
         tc3_scan_bus(&fieldbus->context, "xml");
         continue;
      }
      if (_stricmp(line, "retry") == 0)
      {
         printf("重新扫描并配置从站 ...\n");
         fflush(stdout);
         if (fieldbus_start(fieldbus))
         {
            printf("\n从站已进入 OP，正在进入实时监控 ...\n");
            return 1;
         }
         printf("启动仍然失败，可先 flash 烧录匹配的 ESI 后给从站重新上电。\n");
         continue;
      }
      if (line[0] != '\0')
         printf("未知命令：%s（可用：flash / scan / retry / quit）\n", line);
   }
}

/*
 * 主函数：入口分发
 *   无参数 / --ui                 图形界面版（Win32 + GDI，见 ui/ui_app.c）
 *   --console                     控制台版（自动枚举网卡）
 *   其他参数                      控制台版（参数作为网卡接口名）
 */
int main(int argc, char* argv[])
{
    Fieldbus fieldbus;

    char ifbuf[1024] = { 0 };/* 接口名称缓冲区，由自动检测填写 */

    tc3_varlist_t varlist;
    ec_groupt *grp;
    int prev_lines = 0;
    char last_msg[160];
    char editbuf[EDIT_BUF_SIZE];
    int edit_len = 0, edit_active = 0;
    unsigned long long last_refresh, now;
    long min_time = 0, max_time = 0;
    long long iters = 0;
    int n_out = 0, n_in = 0, i;

    /* ---------- 入口分发：默认进入图形界面 ---------- */
    if (argc <= 1 || strcmp(argv[1], "--ui") == 0)
    {
        return ui_app_run(); /* 图形界面版（Win32 + GDI 自绘） */
    }
    if (strcmp(argv[1], "--console") == 0)
    {
        /* 控制台版：自动枚举网卡 */
        setup_console(); /* 启用 UTF-8 输出与 VT 转义（提前：启动失败进入恢复模式时也需要） */
        if (!auto_select_adapter(ifbuf, sizeof(ifbuf)))
        {
            printf("Failed to select a network adapter.\n");
            return 1;
        }
    }
    else
    {
        /* 控制台版：命令行参数指定的接口 */
        strncpy(ifbuf, argv[1], sizeof(ifbuf) - 1);
        ifbuf[sizeof(ifbuf) - 1] = '\0';
        printf("Using adapter from command line: %s\n", ifbuf);
    }

    fieldbus_initialize(&fieldbus, ifbuf); //初始化现场总线（Fieldbus）结构体

    if (!fieldbus_start(&fieldbus))
    {
        /* 从站未能进入 OP（如 PDO 映射/身份与 ESI 不匹配）：
           不直接退出，进入恢复模式，提供 flash / scan / retry / quit */
        if (!startup_recover(&fieldbus))
        {
            return 1;
        }
    }

    if (fieldbus.context.slavelist[0].hasdc) //如果从站支持DC模式，注意：必须在进入 OPERATIONAL 状态后调用
    {
        printf("DC Mode Activation\n");
        ecx_dcsync0(&fieldbus.context, 1, TRUE, 1000000, 0);//激活从站DC模式，并设置同步周期
    }
    else
    {
        printf("FreeRun Mode\n");
    }

    /* 建立 PDO 变量表（变量名来自 xml/ 下的 ESI，含 IOmap 偏移） */
    tc3_build_varlist(&fieldbus.context, "xml", &varlist);
    /* 检查 xml/ 与从站身份是否匹配（不匹配时实时界面常驻黄色警告） */
    tc3_esi_mismatch_summary(&fieldbus.context, "xml", g_esi_warn,
                             sizeof(g_esi_warn));
    for (i = 0; i < varlist.count; ++i)
    {
        if (varlist.var[i].dir == 0)
            n_out++;
        else
            n_in++;
    }
    printf("\nPDO 变量表: 共 %d 个变量 (输出 %d / 输入 %d)\n", varlist.count, n_out, n_in);
    printf("==============================================================================\n");
    printf(" 实时监控已启动 (每 %d ms 刷新)。命令:\n", LIVE_REFRESH_MS);
    printf("   变量名=值      下发到从站, 如 M1_Speed=-360 或 0x7010:03=100\n");
    printf("   list           重新打印完整扫描表\n");
    printf("   flash          烧录 ESI XML 到从站 EEPROM (TwinCAT Write EEPROM)\n");
    printf("   quit           退出\n");
    printf("==============================================================================\n\n");

    grp = fieldbus.context.grouplist + fieldbus.group;
    last_refresh = GetTickCount64();

    /* 主循环：保持周期通信 + 实时刷新 + 非阻塞命令输入 */
    while (!g_quit)
    {
        int wkc, expected;

        /* 1. 周期交换过程数据 + 2. 状态监视（烧录期间从站处于 INIT，暂停） */
        if (g_flash_state != FL_BURN)
        {
            wkc = fieldbus_roundtrip(&fieldbus);
            iters++;
            if (fieldbus.roundtrip_time > 0)
            {
                if (min_time == 0 || fieldbus.roundtrip_time < min_time)
                    min_time = fieldbus.roundtrip_time;
                if (fieldbus.roundtrip_time > max_time)
                    max_time = fieldbus.roundtrip_time;
            }
            expected = grp->outputsWKC * 2 + grp->inputsWKC;
            /* 烧录对话框激活期间从站可能已被切到 INIT，WKC 提示会刷屏，抑制 */
            if (wkc < expected && g_flash_state == FL_NONE)
            {
                snprintf(last_msg, sizeof(last_msg),
                         "WKC 异常 (%d/%d)，正在尝试恢复", wkc, expected);
            }

            /* 2. 从站状态监视 + 自动恢复（内部按 200ms 节流）
                  烧录流程（含身份不符的二次确认等待）期间必须暂停：
                  否则恢复流程会把从站从 INIT 重新拉回 SAFE-OP/OP，
                  与烧录的"切 INIT + 取 EEPROM 访问权"互相打架 */
            if (g_flash_state == FL_NONE)
                fieldbus_monitor(&fieldbus, wkc, expected, last_msg, sizeof(last_msg));
        }
        else
        {
            wkc = 0;
            expected = 0;
        }

        /* 3. 长时间无法恢复：完整重扫总线（拓扑变化 / 从站重新上电后重配置）
              正在输入命令时先跳过，避免刷掉用户正在编辑的提示行 */
        if (master_need_reinit() && !edit_active)
        {
            master_clear_reinit();
            prev_lines = 0;   /* 扫描输出会滚动，重置界面锚点 */
            fflush(stdout);
            fieldbus_full_reinit(&fieldbus, &varlist, last_msg, sizeof(last_msg));
            grp = fieldbus.context.grouplist + fieldbus.group;
            /* 重新检查 xml/ 与从站身份匹配（身份可能因重新扫描而变化） */
            tc3_esi_mismatch_summary(&fieldbus.context, "xml", g_esi_warn,
                                     sizeof(g_esi_warn));
            last_refresh = 0; /* 立即重画界面 */
        }

        /* 3.5 EEPROM 烧录对话框 / 烧录执行（flash 命令触发） */
        if (g_flash_state == FL_SELECT && !g_flash_shown)
        {
            g_flash_shown = 1;
            prev_lines = 0;
            printf("\n  ================= EEPROM 烧录 =================\n");
            g_flash_nfiles = esi_flash_list_xml("xml", g_flash_files,
                                                ESI_FLASH_MAX_FILES);
            if (g_flash_nfiles == 0)
            {
                printf("  xml 目录下没有可解析的 .xml 设备描述文件\n");
                g_flash_state = FL_NONE;
                g_flash_shown = 0;
            }
            else
            {
                for (i = 0; i < g_flash_nfiles; i++)
                    printf("  [%d] %s\n", i + 1, g_flash_files[i]);
                printf("  输入文件序号选择，输入 c 取消\n");
            }
            fflush(stdout);
        }
        else if (g_flash_state == FL_CONFIRM && !g_flash_shown)
        {
            g_flash_shown = 1;
            printf("\n  即将把 %s 烧录到从站 1 的 EEPROM！\n",
                   g_flash_files[g_flash_pick]);
            printf("  将重写：身份区(字 0x0008-0x000D, 厂商/产品/修订版本) +\n");
            printf("          类别区(字 0x0040 起, STRINGS/GENERAL/FMMU/SM/PDO)。\n");
            printf("  不碰：PDI 配置 + CRC 校验区(0x0000-0x0007)与序列号。\n");
            printf("  烧录后需给从站重新上电生效。\n");
            printf("  确认输入 y，取消输入 n: ");
            fflush(stdout);
        }
        else if (g_flash_state == FL_MISMATCH && !g_flash_shown)
        {
            g_flash_shown = 1;
            printf("\n  \x1b[33m[身份不匹配]\x1b[0m %s\n", g_flash_mismatch);
            printf("  换装新 ESI 时身份本来就不会一致（TwinCAT 同样允许给从站写任意 ESI）。\n");
            printf("  继续将把该 XML 的身份区 + 类别区写入从站 EEPROM，烧录后从站即成为\n");
            printf("  该 XML 描述的设备（需重新上电生效）。请确认 xml 文件选对了！\n");
            printf("  强制继续输入 y，取消输入 n: ");
            fflush(stdout);
        }
        else if (g_flash_state == FL_BURN)
        {
            char ferr[200] = "";
            char fpath[300];
            int r = -1;
            int ok = 0;

            g_flash_shown = 1;
            prev_lines = 0;
            snprintf(fpath, sizeof(fpath), "xml/%s", g_flash_files[g_flash_pick]);

            /* 步骤 1：切 INIT + 强制 EEPROM 归主站（不做这一步会读回全 0） */
            printf("\n  [1/3] 从站切 INIT 并取得 EEPROM 访问权 ...\n");
            fflush(stdout);
            r = esi_flash_begin(&fieldbus.context, 1, ferr, sizeof(ferr));
            if (r == 0)
                printf("        OK\n");

            /* 步骤 2：XML → SII 镜像（身份区 + 类别区）
                  身份与从站不符时先停下来询问用户是否强制烧录 */
            if (r == 0)
            {
                printf("  [2/3] 构建 SII 镜像（XML → 身份区 + 类别区）...\n");
                fflush(stdout);
                r = esi_flash_build_image(&fieldbus.context, 1, fpath, g_flash_img,
                                          ESI_FLASH_MAX_WORDS, g_flash_force,
                                          &g_flash_start, &g_flash_words,
                                          &g_flash_idwords,
                                          ferr, sizeof(ferr));
                if (r == ESI_FLASH_ERR_MISMATCH)
                {
                    /* 交还 EEPROM 访问权，转入二次确认；用户 y 后带
                       force 标志重新进入 FL_BURN */
                    esi_flash_end(&fieldbus.context, 1);
                    snprintf(g_flash_mismatch, sizeof(g_flash_mismatch),
                             "%s", ferr);
                    g_flash_state = FL_MISMATCH;
                    g_flash_shown = 0;
                    printf("        \x1b[33m%s\x1b[0m\n", ferr);
                    fflush(stdout);
                    last_refresh = 0;
                    continue; /* 回到主循环显示确认对话框 */
                }
                if (r == 0)
                    printf("        OK：%d 字身份区 + %d 字类别区（0x%04X - 0x%04X）\n",
                           g_flash_idwords, g_flash_words, g_flash_start,
                           g_flash_start + g_flash_words - 1);
            }

            /* 步骤 3：写入 + 读回校验（身份区有效时先烧身份区） */
            if (r == 0)
            {
                printf("  [3/3] 写入并校验 ...\n");
                fflush(stdout);
                r = esi_flash_burn(&fieldbus.context, 1, g_flash_img,
                                   g_flash_start, g_flash_words,
                                   g_flash_idwords > 0,
                                   flash_progress_cb, NULL, ferr, sizeof(ferr));
                printf("\n");
                if (r >= 0)
                    ok = 1;
            }

            /* 交还 EEPROM 访问权给 PDI（与 SOEM config_init 结束时一致） */
            esi_flash_end(&fieldbus.context, 1);

            if (ok)
            {
                printf("  \x1b[32m烧录完成：共写入并校验 %d 字（含身份区）。\x1b[0m\n", r);
                printf("  注意：ESC 仅在上电复位时从 EEPROM 加载身份与配置区，\n");
                printf("  请给从站重新上电，主站会自动重新检测并匹配新 ESI。\n");
                snprintf(last_msg, sizeof(last_msg), "烧录完成，请给从站重新上电");
            }
            else
            {
                printf("  \x1b[31m烧录失败: %s\x1b[0m\n", ferr);
                snprintf(last_msg, sizeof(last_msg), "烧录失败: %s", ferr);
            }
            g_flash_state = FL_NONE;
            g_flash_shown = 0;
            g_flash_pick = -1;
            g_flash_force = 0;
            g_flash_idwords = 0;
            g_flash_mismatch[0] = '\0';
            fflush(stdout);
            last_refresh = 0; /* 立即重画实时界面 */
        }

        /* 4. 非阻塞键盘输入：编辑期间暂停刷新但保持周期通信 */
        if (!edit_active && _kbhit())
        {
            edit_active = 1;
            edit_len = 0;
            editbuf[0] = '\0';
        }
        if (edit_active)
        {
            while (_kbhit())
            {
                int c = _getch();
                if (c == '\r' || c == '\n')
                {
                    editbuf[edit_len] = '\0';
                    process_command(editbuf, &varlist, grp,
                                    last_msg, sizeof(last_msg));
                    edit_active = 0;
                    edit_len = 0;
                    editbuf[0] = '\0';
                    fflush(stdout);
                    break;
                }
                else if (c == '\b') /* 退格 */
                {
                    if (edit_len > 0)
                    {
                        edit_len--;
                        editbuf[edit_len] = '\0';
                        printf("\b \b");
                        fflush(stdout);
                    }
                }
                else if (c == 0 || c == 0xE0) /* 方向键/功能键前缀，丢弃扫描码 */
                {
                    _getch();
                }
                else if (c >= 32 && c < 127 && edit_len < EDIT_BUF_SIZE - 1)
                {
                    editbuf[edit_len++] = (char)c;
                    editbuf[edit_len] = '\0';
                    putchar(c); /* 手动回显 */
                    fflush(stdout);
                }
            }
        }

        /* 5. 每 200ms 原地刷新实时界面（烧录对话框期间冻结，避免覆盖提示） */
        now = GetTickCount64();
        if (!edit_active && g_flash_state == FL_NONE &&
            now - last_refresh >= LIVE_REFRESH_MS)
        {
            last_refresh = now;
            live_view(&fieldbus, &varlist, &prev_lines, last_msg, editbuf,
                      wkc, expected, fieldbus.roundtrip_time);

            if (g_showlist) /* list 命令：滚动打印完整扫描表 */
            {
                g_showlist = 0;
                prev_lines = 0;
                printf("\n");
                tc3_scan_bus(&fieldbus.context, "xml");
                last_refresh = GetTickCount64();
            }
        }

        osal_usleep(LIVE_CYCLE_US);
    }

    printf("\n");
    printf("\nRoundtrip time (usec): min %ld max %ld, cycles %lld\n",
           min_time, max_time, iters);
    fieldbus_stop(&fieldbus);

    return 0;
}
