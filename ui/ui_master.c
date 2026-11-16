/*
 * ui_master.c - 图形界面版主站后台线程实现
 *
 * 线程职责（对齐控制台版 simple_ng.c 的主循环，但不含任何界面绘制）：
 *   1. 启动序列：ecx_init -> config_init -> PDO 映射 -> DC -> SAFE-OP -> OP
 *   2. 周期过程数据交换（1ms）+ 从站状态监视/自动恢复（200ms 节流）
 *   3. 长时间失联的完整重扫（拓扑变化兜底）
 *   4. EEPROM 烧录流程（选文件 -> 构建镜像 -> 身份不符确认 -> 烧录+校验）
 *   5. 响应 UI 命令：写变量 / DC / FreeRUN / 重扫 / 退出
 */

#include "ui_master.h"
#include "esi_flash.h"
#include "ui_lang.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ---------------- 模块内部状态 ---------------- */

static CRITICAL_SECTION g_cs;          /* 保护快照与命令队列 */
static int g_cs_init = 0;

/* 界面语言：0 = 中文, 1 = English（TR 宏使用，见 ui_lang.h） */
int ui_lang = 0;

static HANDLE g_thread = NULL;         /* 主站线程句柄 */
static volatile int g_run = 0;         /* 线程运行标志 */

static ui_snap_t g_snap;               /* 快照（g_cs 保护） */
static int g_cyc_us = 0;               /* 实测主循环平均周期（微秒，200ms 窗口） */
static int g_cyc_min = 0, g_cyc_max = 0;   /* 窗口内最小/最大（抖动范围） */

/* 主站消息日志环（g_cs 保护），UI 增量拉取用 */
#define MSGLOG_MAX 64
static ui_msgitem_t g_msglog[MSGLOG_MAX];
static int g_msglog_head = 0;
static unsigned g_msg_seq = 0;

#define UICMD_QUEUE_MAX 16
static ui_cmd_msg_t g_queue[UICMD_QUEUE_MAX];
static int g_qhead = 0, g_qtail = 0;   /* 环形队列（g_cs 保护） */

/* 从站模式偏好（g_cs 保护）：1=DC, 0=FreeRUN, -1=未指定（自动）。
 * 启动序列按此偏好决定是否激活 DC，界面把它持久化实现"记住从站模式"。 */
static int g_mode_pref = -1;

/* 主站上下文：仅主站线程访问 */
static Fieldbus g_fb;
static tc3_varlist_t g_vl;             /* 主站线程自己的变量表 */

/* 烧录子状态（主站线程私有） */
#define FLPH_IDLE 0
#define FLPH_WORK 1   /* 构建镜像 / 烧录中 */
#define FLPH_WAIT 2   /* 身份不符，等待 UI 确认 */
static int g_flash_phase = FLPH_IDLE;
static char g_flash_file[64] = "";
static int g_flash_slave = 1;          /* 烧录目标从站序号（1 起） */
static int g_flash_force = 0;

/* ---------------- 内部工具 ---------------- */

/* 把字符串净化为可安全显示的内容：仅保留可打印 ASCII 与合法 UTF-8 序列，
   其余字节替换为 '?'。EEPROM 被烧错后 SOEM 从 SII 读到的从站名可能是
   任意二进制数据，不净化直接进日志/界面会显示一整行乱码 */
static void sanitize_text(char *s)
{
   unsigned char *p = (unsigned char *)s;
   unsigned char *w = (unsigned char *)s;

   while (*p)
   {
      unsigned char c = *p;
      int len = 1;

      if (c < 0x80)
      {
         if (c < 0x20 && c != '\t' && c != '\n')
            c = '?';
      }
      else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
         len = 2;
      else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80)
         len = 3;
      else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
         len = 4;
      else
         c = '?';

      *w++ = c;
      if (len > 1)
      {
         int k;
         for (k = 1; k < len; ++k)
            *w++ = p[k];
      }
      p += len;
   }
   *w = '\0';
}

static char g_msgdup_text[sizeof(g_snap.msg)];   /* 上一条入环消息（重复抑制用） */
static unsigned long long g_msgdup_tick = 0;

static void snap_set_msg(const char *fmt, ...)
{
   va_list ap;
   char buf[sizeof(g_snap.msg)];
   int dup = 0;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   sanitize_text(buf);
   EnterCriticalSection(&g_cs);
   snprintf(g_snap.msg, sizeof(g_snap.msg), "%s", buf);
   /* 同步记入消息日志环（UI 按 seq 拉取，保证不漏）。
    * 抑制刷屏：与上一条完全相同且间隔 < 2s 的消息只更新状态栏，不入环 */
   if (strcmp(buf, g_msgdup_text) == 0 &&
       GetTickCount64() - g_msgdup_tick < 2000)
   {
      dup = 1;
   }
   else
   {
      g_msg_seq++;
      {
         ui_msgitem_t *it = &g_msglog[g_msglog_head];
         it->seq = g_msg_seq;
         snprintf(it->text, sizeof(it->text), "%s", buf);
         g_msglog_head = (g_msglog_head + 1) % MSGLOG_MAX;
      }
      snprintf(g_msgdup_text, sizeof(g_msgdup_text), "%s", buf);
      g_msgdup_tick = GetTickCount64();
   }
   (void)dup;
   LeaveCriticalSection(&g_cs);
}

static void snap_set_state(ui_mst_state_t st)
{
   EnterCriticalSection(&g_cs);
   g_snap.mstate = st;
   LeaveCriticalSection(&g_cs);
}

/* ---------------- CiA402 驱动控制 ---------------- */

/* 每从站 CiA402 对象在变量表（g_vl）中的下标 + 1（0 = 无该对象）。
 * 变量表每次重建后由 c402_build_objlist 重新定位 */
static int g_c402_cw[MASTER_MAXSLAVE + 1];    /* 0x6040 控制字（输出） */
static int g_c402_sw[MASTER_MAXSLAVE + 1];    /* 0x6041 状态字（输入） */
static int g_c402_mset[MASTER_MAXSLAVE + 1];  /* 0x6060 运行模式设置（输出） */
static int g_c402_mget[MASTER_MAXSLAVE + 1];  /* 0x6061 运行模式显示（输入） */

/* 控制字序列（如一键使能 0x06 -> 0x07 -> 0x0F）的逐周期推进 */
#define C402_SEQ_MAX 4
typedef struct
{
   int active;               /* 1 = 序列执行中 */
   int cur, n;               /* 当前步 / 总步数 */
   int cw[C402_SEQ_MAX];     /* 每步写入的控制字值 */
   int dly[C402_SEQ_MAX];    /* 每步执行后的延时（ms） */
} c402_seq_t;
static c402_seq_t g_c402_seq[MASTER_MAXSLAVE + 1];
static ULONGLONG g_c402_seq_t[MASTER_MAXSLAVE + 1];   /* 下一步执行时刻 */

/* 变量表重建后重新定位 CiA402 对象并复位未完成的序列 */
static void c402_build_objlist(const tc3_varlist_t *vl)
{
   int v;

   memset(g_c402_cw, 0, sizeof(g_c402_cw));
   memset(g_c402_sw, 0, sizeof(g_c402_sw));
   memset(g_c402_mset, 0, sizeof(g_c402_mset));
   memset(g_c402_mget, 0, sizeof(g_c402_mget));
   memset(g_c402_seq, 0, sizeof(g_c402_seq));

   for (v = 0; v < vl->count && v < TC3_MAX_VARS; ++v)
   {
      const tc3_variable_t *var = &vl->var[v];
      int s = var->slave;
      if (s < 1 || s > MASTER_MAXSLAVE) continue;
      if (var->index == 0x6040 && var->sub == 0 && var->dir == 0 && !g_c402_cw[s])
         g_c402_cw[s] = v + 1;
      else if (var->index == 0x6041 && var->sub == 0 && var->dir == 1 && !g_c402_sw[s])
         g_c402_sw[s] = v + 1;
      else if (var->index == 0x6060 && var->sub == 0 && var->dir == 0 && !g_c402_mset[s])
         g_c402_mset[s] = v + 1;
      else if (var->index == 0x6061 && var->sub == 0 && var->dir == 1 && !g_c402_mget[s])
         g_c402_mget[s] = v + 1;
   }
}

/* 按状态字低 6 位解码 CiA402 状态机状态（DSP402 标准状态表） */
static int c402_decode_state(uint16 sw)
{
   uint16 low = sw & 0x000F;

   if (low == 0x0000) return (sw & 0x0040) ? C402_ST_SODIS : C402_ST_NOTREADY;
   if (low == 0x0001) return C402_ST_RTSO;
   if (low == 0x0003) return C402_ST_SON;
   if (low == 0x0007) return (sw & 0x0010) ? C402_ST_OPEN : C402_ST_QSTOP;
   if (low == 0x000F) return (sw & 0x0010) ? C402_ST_FREA : C402_ST_FAULT;
   return C402_ST_NONE;
}

/* 启动一个控制字序列（第一步在下次 poll 立即执行） */
static void c402_start_seq(int sl, const int *cw, const int *dly, int n)
{
   c402_seq_t *q;

   if (sl < 1 || sl > MASTER_MAXSLAVE || n > C402_SEQ_MAX) return;
   q = &g_c402_seq[sl];
   memset(q, 0, sizeof(*q));
   q->n = n;
   memcpy(q->cw, cw, n * sizeof(int));
   memcpy(q->dly, dly, n * sizeof(int));
   q->active = 1;
   q->cur = 0;
   g_c402_seq_t[sl] = 0;
}

/* 主循环每周期推进序列：写 outputs 缓冲，随本周期过程数据下发到从站 */
static void c402_seq_poll(ecx_contextt *ctx)
{
   int i;
   ec_groupt *grp = ctx->grouplist + g_fb.group;
   char msg[160];

   if (grp->outputs == NULL) return;
   for (i = 1; i <= ctx->slavecount && i <= MASTER_MAXSLAVE; ++i)
   {
      c402_seq_t *q = &g_c402_seq[i];
      if (!q->active || g_c402_cw[i] <= 0) continue;
      if (GetTickCount64() < g_c402_seq_t[i]) continue;
      write_var_value(&g_vl.var[g_c402_cw[i] - 1], grp->outputs,
                      q->cw[q->cur], msg, sizeof(msg));
      q->cur++;
      if (q->cur >= q->n)
         q->active = 0;
      else
         g_c402_seq_t[i] = GetTickCount64() + (ULONGLONG)q->dly[q->cur - 1];
   }
}

/* 处理 UICMD_CIA402（主站线程上下文） */
static void handle_cia402(ecx_contextt *ctx, const ui_cmd_msg_t *m)
{
   ec_groupt *grp = ctx->grouplist + g_fb.group;
   int sl = m->slave;
   char msg[160];

   if (sl < 1) sl = 1;
   if (ctx->slavecount > 0 && sl > ctx->slavecount) sl = ctx->slavecount;
   if (g_c402_cw[sl] <= 0 || g_c402_sw[sl] <= 0)
   {
      snap_set_msg(TR("从站 %d 无 CiA402 驱动对象（0x6040/0x6041），无法控制",
                      "Slave %d has no CiA402 drive objects (0x6040/0x6041)"), sl);
      return;
   }
   if (grp->outputs == NULL)
   {
      snap_set_msg(TR("输出缓冲未就绪", "Output buffer not ready"));
      return;
   }

   switch (m->var_index)
   {
      case C402_ACT_RESET:
      {
         static const int cw[] = { 0x0080, 0x0000 };
         static const int dly[] = { 100, 0 };
         c402_start_seq(sl, cw, dly, 2);
         snap_set_msg(TR("从站 %d：故障复位（0x6040 = 0x80 脉冲）",
                         "Slave %d: fault reset (0x6040 = 0x80 pulse)"), sl);
         break;
      }
      case C402_ACT_DISVOLT:
      {
         static const int cw[] = { 0x0000 };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：禁止电压（控制字 0x0000）",
                         "Slave %d: disable voltage (CW 0x0000)"), sl);
         break;
      }
      case C402_ACT_QSTOP:
      {
         static const int cw[] = { 0x0002 };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：快速停止（控制字 0x0002）",
                         "Slave %d: quick stop (CW 0x0002)"), sl);
         break;
      }
      case C402_ACT_SHUTDOWN:
      {
         static const int cw[] = { 0x0006 };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：上电就绪（控制字 0x0006）",
                         "Slave %d: shutdown (CW 0x0006)"), sl);
         break;
      }
      case C402_ACT_SWITCHON:
      {
         static const int cw[] = { 0x0007 };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：开通（控制字 0x0007）",
                         "Slave %d: switch on (CW 0x0007)"), sl);
         break;
      }
      case C402_ACT_ENABLE:
      {
         static const int cw[] = { 0x000F };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：运行使能（控制字 0x000F）",
                         "Slave %d: enable operation (CW 0x000F)"), sl);
         break;
      }
      case C402_ACT_DISABLE:
      {
         static const int cw[] = { 0x0007 };
         static const int dly[] = { 0 };
         c402_start_seq(sl, cw, dly, 1);
         snap_set_msg(TR("从站 %d：关使能（控制字 0x0007）",
                         "Slave %d: disable operation (CW 0x0007)"), sl);
         break;
      }
      case C402_ACT_AUTOENABLE:
      {
         static const int cw[] = { 0x0006, 0x0007, 0x000F };
         static const int dly[] = { 60, 60, 0 };
         c402_start_seq(sl, cw, dly, 3);
         snap_set_msg(TR("从站 %d：一键使能（就绪→开通→运行使能）",
                         "Slave %d: auto enable (shutdown->switch on->enable)"), sl);
         break;
      }
      case C402_ACT_MODE:
         if (g_c402_mset[sl] > 0)
         {
            write_var_value(&g_vl.var[g_c402_mset[sl] - 1], grp->outputs,
                            m->value, msg, sizeof(msg));
            snap_set_msg(TR("从站 %d：运行模式已下发（模式码 %d）",
                            "Slave %d: mode of operation set (code %d)"),
                         sl, (int)m->value);
         }
         else
            snap_set_msg(TR("从站 %d 无运行模式对象（0x6060），无法切换",
                            "Slave %d has no mode object (0x6060)"), sl);
         break;
      default:
         break;
   }
}

/* 把从站状态、变量值等刷新进快照（仅主站线程调用） */
static void snap_update(ecx_contextt *ctx, const tc3_varlist_t *vl,
                        ec_groupt *grp, int wkc, int expected)
{
   int i;

   EnterCriticalSection(&g_cs);
   g_snap.wkc = wkc;
   g_snap.expected = expected;
   g_snap.rt_us = g_fb.roundtrip_time;
   g_snap.cycle_us = g_cyc_us;
   g_snap.cycle_min_us = g_cyc_min;
   g_snap.cycle_max_us = g_cyc_max;
   g_snap.dctime = (long long)ctx->DCtime;

   /* 先刷新 PDO 变量值（CiA402 状态字/模式显示从中读取） */
   for (i = 0; i < vl->count && i < TC3_MAX_VARS; ++i)
   {
      const tc3_variable_t *v = &vl->var[i];
      if (v->dir == 0 && grp->outputs != NULL)
         g_snap.oval[i] = read_var_value(v, grp->outputs);
      else if (v->dir == 1 && grp->inputs != NULL)
         g_snap.ival[i] = read_var_value(v, grp->inputs);
      else
      {
         g_snap.oval[i] = 0;
         g_snap.ival[i] = 0;
      }
   }

   g_snap.slavecount = ctx->slavecount;
   for (i = 1; i <= ctx->slavecount && i <= MASTER_MAXSLAVE; ++i)
   {
      ec_slavet *sl = &ctx->slavelist[i];
      ui_slave_info_t *ui = &g_snap.slave[i];
      strncpy(ui->name, sl->name, TC3_NAME_LEN - 1);
      ui->name[TC3_NAME_LEN - 1] = '\0';
      sanitize_text(ui->name);   /* EEPROM 烧错时从站名可能是二进制垃圾 */
      ui->state = sl->state;
      slave_state_label(sl->state, ui->label, sizeof(ui->label));
      ui->islost = sl->islost ? 1 : 0;
      ui->alstatus = sl->ALstatuscode;
      ui->linkstat = master_linkstat(i);
      ui->hasdc = sl->hasdc ? 1 : 0;

      /* CiA402 驱动状态（0x6041 状态字 + 0x6061 模式显示） */
      ui->c402 = (g_c402_cw[i] > 0 && g_c402_sw[i] > 0) ? 1 : 0;
      if (ui->c402)
      {
         ui->c402_sw = (uint16)(g_snap.ival[g_c402_sw[i] - 1] & 0xFFFF);
         ui->c402_state = c402_decode_state(ui->c402_sw);
         ui->c402_mode = (g_c402_mget[i] > 0)
                         ? (int)(g_snap.ival[g_c402_mget[i] - 1] & 0xFF) : -1;
      }
      else
      {
         ui->c402_sw = 0;
         ui->c402_state = C402_ST_NONE;
         ui->c402_mode = -1;
      }
   }
   LeaveCriticalSection(&g_cs);
}

/* 变量表重建后同步进快照（仅主站线程调用）。
   注意：这里同时构建主站线程私有的 g_vl（写命令与数值刷新都用它）
   和 UI 显示用的 g_snap.vl——先建 g_vl 再整体拷贝，两表永远一致。
   首次启动路径只调用本函数，不经过 fieldbus_full_reinit，
   因此 g_vl 的构建必须放在这里，否则写命令和数值刷新全部失效。 */
static void snap_sync_varlist(const char *xml_dir, ecx_contextt *ctx)
{
   tc3_build_varlist(ctx, xml_dir, &g_vl);
   c402_build_objlist(&g_vl);   /* 重新定位 CiA402 驱动对象 */
   EnterCriticalSection(&g_cs);
   g_snap.vl = g_vl;
   memset(g_snap.oval, 0, sizeof(g_snap.oval));
   memset(g_snap.ival, 0, sizeof(g_snap.ival));
   /* 变量名/ESI 警告同样可能带 EEPROM 侧的二进制内容，一并净化 */
   {
      int i;
      for (i = 0; i < g_snap.vl.count && i < TC3_MAX_VARS; ++i)
         sanitize_text(g_snap.vl.var[i].name);
      sanitize_text(g_snap.esi_warn);
   }
   LeaveCriticalSection(&g_cs);
   tc3_esi_mismatch_summary(ctx, xml_dir, g_snap.esi_warn, sizeof(g_snap.esi_warn));
}

/* 取一条命令（无命令返回 id==UICMD_NONE） */
static ui_cmd_msg_t cmd_pop(void)
{
   ui_cmd_msg_t m;
   m.id = UICMD_NONE;
   EnterCriticalSection(&g_cs);
   if (g_qhead != g_qtail)
   {
      m = g_queue[g_qhead];
      g_qhead = (g_qhead + 1) % UICMD_QUEUE_MAX;
   }
   LeaveCriticalSection(&g_cs);
   return m;
}

/* ---------------- 烧录进度回调（主站线程上下文） ---------------- */

static void ui_flash_progress_cb(void *user, int done, int total)
{
   (void)user;
   EnterCriticalSection(&g_cs);
   g_snap.flash_done = done;
   g_snap.flash_total = total;
   LeaveCriticalSection(&g_cs);
}

/* ---------------- 命令处理 ---------------- */

/* 写输出变量（主站线程上下文，直接操作 grp->outputs） */
static void handle_write_var(ecx_contextt *ctx, const tc3_varlist_t *vl, int idx, long value)
{
   ec_groupt *grp = ctx->grouplist + g_fb.group;
   char msg[160];

   if (idx < 0 || idx >= vl->count)
   {
      snap_set_msg(TR("写入失败：变量索引 %d 超出变量表（共 %d 项）",
                      "Write failed: var index %d out of range (%d vars)"), idx, vl->count);
      return;
   }
   if (vl->var[idx].dir != 0)
   {
      snap_set_msg(TR("%s 是输入(TxPDO)变量，只读",
                      "%s is an input (TxPDO) variable, read-only"), vl->var[idx].name);
      return;
   }
   if (grp->outputs == NULL)
   {
      snap_set_msg(TR("输出缓冲未就绪", "Output buffer not ready"));
      return;
   }
   write_var_value(&vl->var[idx], grp->outputs, value, msg, sizeof(msg));
   snap_set_msg("%s", msg);
}

/* DC / FreeRUN 切换（主站线程上下文） */
static void handle_mode(ecx_contextt *ctx, int dc_on)
{
   int i, n = 0;

   if (g_flash_phase != FLPH_IDLE)
      return;
   for (i = 1; i <= ctx->slavecount; ++i)
   {
      if (ctx->slavelist[i].hasdc)
      {
         ecx_dcsync0(ctx, (uint16)i, dc_on ? TRUE : FALSE, 1000000, 0);
         n++;
      }
   }
   EnterCriticalSection(&g_cs);
   g_snap.dc_active = dc_on;
   g_mode_pref = dc_on;   /* 记住用户选择，重连时沿用 */
   LeaveCriticalSection(&g_cs);
   if (n > 0 && dc_on)
      snap_set_msg(TR("DC 同步已激活（%d 个从站，周期 1ms）",
                      "DC sync active (%d slaves, 1ms cycle)"), n);
   else if (dc_on)
      snap_set_msg(TR("无从站支持 DC，仍为 FreeRUN",
                      "No slave supports DC, staying in FreeRUN"));
   else
      snap_set_msg(TR("已切换为 FreeRUN 模式", "Switched to FreeRUN mode"));
}

/* 烧录流程推进（主站线程上下文，仅 flash_phase != IDLE 时调用） */
static void flash_step(ecx_contextt *ctx)
{
   char ferr[200] = "";
   char fpath[300];
   static uint16 img[ESI_FLASH_MAX_WORDS];
   static int start, words, idwords;
   int r;

   snprintf(fpath, sizeof(fpath), "xml/%s", g_flash_file);

   if (g_flash_force >= 0) /* 正常路径：begin -> build -> burn */
   {
      EnterCriticalSection(&g_cs);
      g_snap.flash_phase = FLPH_WORK;
      g_snap.flash_done = 0;
      g_snap.flash_total = 0;
      g_snap.flash_result = 0;
      g_snap.flash_err[0] = '\0';
      g_snap.flash_slave = g_flash_slave;
      strncpy(g_snap.flash_file, g_flash_file, sizeof(g_snap.flash_file) - 1);
      LeaveCriticalSection(&g_cs);
      snap_set_msg(TR("EEPROM 烧录开始：从站 %d，%s",
                      "EEPROM flash started: slave %d, %s"), g_flash_slave, g_flash_file);

      r = esi_flash_begin(ctx, g_flash_slave, ferr, sizeof(ferr));
      if (r != 0)
      {
         EnterCriticalSection(&g_cs);
         g_snap.flash_result = -1;
         snprintf(g_snap.flash_err, sizeof(g_snap.flash_err), "%s", ferr);
         LeaveCriticalSection(&g_cs);
         snap_set_msg(TR("烧录失败：%s", "Flash failed: %s"), ferr);
         g_flash_phase = FLPH_IDLE;
         EnterCriticalSection(&g_cs);
         g_snap.flash_phase = FLPH_IDLE;
         g_snap.flash_seq++;
         LeaveCriticalSection(&g_cs);
         return;
      }

      r = esi_flash_build_image(ctx, g_flash_slave, fpath, img, ESI_FLASH_MAX_WORDS,
                                g_flash_force, &start, &words, &idwords,
                                ferr, sizeof(ferr));
      if (r == ESI_FLASH_ERR_MISMATCH)
      {
         esi_flash_end(ctx, g_flash_slave);
         g_flash_force = -1; /* -1 = 等待用户确认强制烧录 */
         EnterCriticalSection(&g_cs);
         g_snap.flash_phase = FLPH_WAIT;
         snprintf(g_snap.flash_mismatch_msg, sizeof(g_snap.flash_mismatch_msg),
                  TR("从站 %d：%s", "Slave %d: %s"), g_flash_slave, ferr);
         LeaveCriticalSection(&g_cs);
         snap_set_msg(TR("从站 %d 的 XML 身份不符，等待确认",
                         "Slave %d: XML identity mismatch, waiting for confirm"),
                      g_flash_slave);
         return;
      }
      if (r != 0)
      {
         esi_flash_end(ctx, g_flash_slave);
         EnterCriticalSection(&g_cs);
         g_snap.flash_result = -1;
         snprintf(g_snap.flash_err, sizeof(g_snap.flash_err), "%s", ferr);
         LeaveCriticalSection(&g_cs);
         snap_set_msg(TR("烧录失败：%s", "Flash failed: %s"), ferr);
         g_flash_phase = FLPH_IDLE;
         EnterCriticalSection(&g_cs);
         g_snap.flash_phase = FLPH_IDLE;
         g_snap.flash_seq++;
         LeaveCriticalSection(&g_cs);
         return;
      }

      r = esi_flash_burn(ctx, g_flash_slave, img, start, words, idwords > 0,
                         ui_flash_progress_cb, NULL, ferr, sizeof(ferr));
      esi_flash_end(ctx, g_flash_slave);

      EnterCriticalSection(&g_cs);
      if (r >= 0)
      {
         g_snap.flash_result = 1;
         snap_set_msg(TR("烧录完成：从站 %d 写入并校验 %d 字，请给该从站重新上电",
                         "Flash done: slave %d written and verified %d words,"
                         " please power-cycle the slave"),
                      g_flash_slave, r);
      }
      else
      {
         g_snap.flash_result = -1;
         snprintf(g_snap.flash_err, sizeof(g_snap.flash_err), "%s", ferr);
         snap_set_msg(TR("烧录失败：%s", "Flash failed: %s"), ferr);
      }
      g_snap.flash_phase = FLPH_IDLE;
      g_snap.flash_seq++;
      LeaveCriticalSection(&g_cs);
      g_flash_phase = FLPH_IDLE;
      g_flash_force = 0;
   }
}

/* ---------------- 启动序列 ---------------- */

/* 启动后公共步骤：DC 激活（遵循保存的模式偏好）、变量表、快照同步 */
static void start_post_sequence(ecx_contextt *ctx)
{
   int pref;

   EnterCriticalSection(&g_cs);
   g_snap.dc_active = 0;
   pref = g_mode_pref;
   LeaveCriticalSection(&g_cs);

   if (ctx->slavelist[0].hasdc && pref != 0)
   {
      ecx_dcsync0(ctx, 1, TRUE, 1000000, 0); /* 进入 OP 后激活 DC */
      EnterCriticalSection(&g_cs);
      g_snap.dc_active = 1;
      LeaveCriticalSection(&g_cs);
      snap_set_msg(TR("主站运行中：DC 模式（1ms 同步）",
                      "Master running: DC mode (1ms sync)"));
   }
   else if (pref == 0)
   {
      snap_set_msg(TR("主站运行中：FreeRUN 模式（已按保存的设置）",
                      "Master running: FreeRUN mode (saved setting)"));
   }
   else
   {
      snap_set_msg(TR("主站运行中：FreeRUN 模式",
                      "Master running: FreeRUN mode"));
   }
}

/* ---------------- 主站线程 ---------------- */

/* 目标从站序号合法化：至少 1，最多不超过当前扫描到的从站数 */
static int flash_slave_clamp(ecx_contextt *ctx, int s)
{
   if (s < 1)
      s = 1;
   if (ctx->slavecount > 0 && s > ctx->slavecount)
      s = ctx->slavecount;
   return s;
}

static DWORD WINAPI ui_master_thread(LPVOID arg)
{
   const char *adapter = (const char *)arg;
   ecx_contextt *ctx = &g_fb.context;
   ec_groupt *grp;
   int wkc = 0, expected = 0;
   long long iters = 0;
   ULONGLONG cyc_t0_tick = 0;     /* 200ms 统计窗口起点（GetTickCount64） */
   LARGE_INTEGER cyc_qprev;       /* 上次循环 QPC 计数（实测周期用） */
   LARGE_INTEGER g_qpf;           /* QPC 频率（每秒计数） */
   int cyc_min = 0, cyc_max = 0;  /* 窗口内最小/最大周期（微秒） */
   int cyc_sum = 0, cyc_n = 0;    /* 窗口内周期总和 / 样本数 */

   memset(&cyc_qprev, 0, sizeof(cyc_qprev));
   QueryPerformanceFrequency(&g_qpf);

   EnterCriticalSection(&g_cs);
   strncpy(g_snap.adapter, adapter, sizeof(g_snap.adapter) - 1);
   g_snap.slavecount = 0;
   g_snap.vl.count = 0;
   g_snap.esi_warn[0] = '\0';
   g_snap.dc_active = 0;
   g_snap.cycle_us = 0;
   g_cyc_us = 0;
   g_cyc_min = 0;
   g_cyc_max = 0;
   g_snap.flash_result = 0;
   LeaveCriticalSection(&g_cs);

   /* fieldbus_initialize 需要进程生命周期内有效的可写字符串 */
   {
      static char ifacebuf[256];
      strncpy(ifacebuf, adapter, sizeof(ifacebuf) - 1);
      ifacebuf[sizeof(ifacebuf) - 1] = '\0';
      fieldbus_initialize(&g_fb, ifacebuf);
   }

   snap_set_state(MST_STARTING);
   snap_set_msg(TR("正在初始化主站并扫描从站...",
                   "Initializing master and scanning bus..."));

   if (!fieldbus_start(&g_fb))
   {
      snap_set_state(MST_STARTFAIL);
      snap_set_msg(TR("启动失败：从站未能进入 OP（可烧录匹配的 ESI 后重试）",
                      "Start failed: slaves did not enter OP"
                      " (flash a matching ESI and retry)"));
      /* 启动失败后仍保持线程存活，允许重试 / 烧录；不进入周期交换 */
      while (g_run)
      {
         ui_cmd_msg_t m = cmd_pop();
         if (m.id == UICMD_QUIT)
            break;
         if (m.id == UICMD_RESCAN)
         {
            snap_set_state(MST_STARTING);
            snap_set_msg(TR("正在重试启动...", "Retrying start..."));
            if (fieldbus_start(&g_fb))
            {
               snap_set_state(MST_RUNNING);
               goto running_loop;
            }
            snap_set_state(MST_STARTFAIL);
            snap_set_msg(TR("启动失败：从站未能进入 OP（可烧录匹配的 ESI 后重试）",
                            "Start failed: slaves did not enter OP"
                            " (flash a matching ESI and retry)"));
         }
         if (m.id == UICMD_FLASH_FILE)
         {
            /* 启动失败（从站在网但未 OP）时也允许烧录 */
            strncpy(g_flash_file, m.name, sizeof(g_flash_file) - 1);
            g_flash_file[sizeof(g_flash_file) - 1] = '\0';
            g_flash_slave = flash_slave_clamp(ctx, m.slave);
            g_flash_force = 0;
            g_flash_phase = FLPH_WORK;
            flash_step(ctx);
         }
         Sleep(50);
      }
      return 0;
   }

running_loop:
   start_post_sequence(ctx);
   snap_sync_varlist("xml", ctx);
   snap_set_state(MST_RUNNING);

   grp = ctx->grouplist + g_fb.group;

   /* 周期主循环：1ms 交换 + 监视 + 命令 + 烧录 + 重扫 */
   while (g_run)
   {
      ui_cmd_msg_t m;

      /* 1) 命令处理 */
      while ((m = cmd_pop()).id != UICMD_NONE)
      {
         switch (m.id)
         {
            case UICMD_QUIT:
               g_run = 0;
               break;
            case UICMD_WRITE_VAR:
               if (g_flash_phase == FLPH_IDLE)
                  handle_write_var(ctx, &g_vl, m.var_index, m.value);
               break;
            case UICMD_MODE_DC:
               handle_mode(ctx, 1);
               break;
            case UICMD_MODE_FREERUN:
               handle_mode(ctx, 0);
               break;
            case UICMD_RESCAN:
               if (g_flash_phase == FLPH_IDLE)
               {
                  char mbuf[160];
                  snap_set_msg(TR("正在重新扫描总线...",
                                  "Rescanning the bus..."));
                  fieldbus_full_reinit(&g_fb, &g_vl, mbuf, sizeof(mbuf));
                  snap_set_msg("%s", mbuf);
                  snap_sync_varlist("xml", ctx);
                  grp = ctx->grouplist + g_fb.group;
               }
               break;
            case UICMD_FLASH_FILE:
               if (g_flash_phase == FLPH_IDLE)
               {
                  strncpy(g_flash_file, m.name, sizeof(g_flash_file) - 1);
                  g_flash_file[sizeof(g_flash_file) - 1] = '\0';
                  g_flash_slave = flash_slave_clamp(ctx, m.slave);
                  g_flash_force = 0;
                  g_flash_phase = FLPH_WORK;
               }
               break;
            case UICMD_FLASH_YES:
               if (g_flash_phase == FLPH_WAIT)
               {
                  g_flash_force = 1; /* 强制烧录：重新走 begin/build/burn */
                  g_flash_phase = FLPH_WORK;
               }
               break;
            case UICMD_FLASH_NO:
               if (g_flash_phase == FLPH_WAIT)
               {
                  g_flash_phase = FLPH_IDLE;
                  g_flash_force = 0;
                  EnterCriticalSection(&g_cs);
                  g_snap.flash_phase = FLPH_IDLE;
                  g_snap.flash_seq++;
                  LeaveCriticalSection(&g_cs);
                  snap_set_msg(TR("已取消烧录", "Flashing cancelled"));
               }
               break;
            case UICMD_CIA402:
               if (g_flash_phase == FLPH_IDLE)
                  handle_cia402(ctx, &m);
               break;
            default:
               break;
         }
         if (!g_run)
            break;
      }
      if (!g_run)
         break;

      /* 2) 烧录流程（激活期间暂停周期交换与监视，与控制台版一致） */
      if (g_flash_phase == FLPH_WORK)
      {
         flash_step(ctx);
         continue;
      }
      if (g_flash_phase == FLPH_WAIT)
      {
         Sleep(20); /* 等待 UI 确认 */
         continue;
      }

      /* 3) 周期过程数据交换 */
      c402_seq_poll(ctx);   /* CiA402 控制字序列（写入即随本周期下发） */
      wkc = fieldbus_roundtrip(&g_fb);
      {
         /* 实测主循环周期：相邻两次交换间隔，按 200ms 窗口统计
          * 平均值 + 最小/最大（抖动范围），每 200ms 刷新进快照。
          * 用 QueryPerformanceCounter 保证微秒级分辨率 */
         LARGE_INTEGER qnow;
         QueryPerformanceCounter(&qnow);
         if (cyc_qprev.QuadPart != 0)
         {
            long long us = (qnow.QuadPart - cyc_qprev.QuadPart) *
                           1000000 / g_qpf.QuadPart;
            if (us > 100 && us < 100000)
            {
               if (cyc_n == 0 || us < cyc_min) cyc_min = (int)us;
               if (cyc_n == 0 || us > cyc_max) cyc_max = (int)us;
               cyc_sum += (int)us;
               cyc_n++;
            }
         }
         cyc_qprev = qnow;

         /* 每 200ms 结算一次：平均周期 + 抖动范围写入快照
          * 窗口计时用 GetTickCount64（绝对时间差用 32 位 long 会溢出） */
         {
            ULONGLONG tk = GetTickCount64();
            if (tk - cyc_t0_tick >= 200)
            {
               if (cyc_n > 0)
               {
                  g_cyc_us  = cyc_sum / cyc_n;
                  g_cyc_min = cyc_min;
                  g_cyc_max = cyc_max;
               }
               cyc_t0_tick = tk;
               cyc_sum = 0; cyc_n = 0; cyc_min = 0; cyc_max = 0;
            }
         }
      }
      iters++;
      expected = grp->outputsWKC * 2 + grp->inputsWKC;
      if (wkc < expected)
         snap_set_msg(TR("WKC 异常 (%d/%d)，正在尝试恢复",
                         "WKC error (%d/%d), trying to recover"), wkc, expected);

      /* 4) 从站状态监视 + 自动恢复（内部 200ms 节流） */
      {
         char mbuf[160];
         mbuf[0] = '\0';
         fieldbus_monitor(&g_fb, wkc, expected, mbuf, sizeof(mbuf));
         if (mbuf[0])
            snap_set_msg("%s", mbuf);
      }

      /* 5) 长时间无法恢复：完整重扫 */
      if (master_need_reinit())
      {
         char mbuf[160];
         mbuf[0] = '\0';
         master_clear_reinit();
         fieldbus_full_reinit(&g_fb, &g_vl, mbuf, sizeof(mbuf));
         snap_set_msg("%s", mbuf);
         snap_sync_varlist("xml", ctx);
         grp = ctx->grouplist + g_fb.group;
      }

      /* 6) 快照刷新 */
      snap_update(ctx, &g_vl, grp, wkc, expected);

      osal_usleep(1000); /* 1ms 周期 */
   }

   (void)iters;
   /* 优雅停止：从站回 INIT，关闭网络 */
   fieldbus_stop(&g_fb);
   snap_set_state(MST_IDLE);
   snap_set_msg(TR("主站已停止", "Master stopped"));
   return 0;
}

/* ---------------- 对外接口 ---------------- */

void ui_master_start(const char *adapter)
{
   static char adapter_buf[256]; /* 线程读取，进程生命周期内有效 */

   if (!g_cs_init)
      return;

   ui_master_stop(); /* 若已有线程在跑，先停掉 */

   strncpy(adapter_buf, adapter, sizeof(adapter_buf) - 1);
   adapter_buf[sizeof(adapter_buf) - 1] = '\0';

   EnterCriticalSection(&g_cs);
   memset(&g_snap, 0, sizeof(g_snap));
   g_snap.mstate = MST_STARTING;
   snprintf(g_snap.msg, sizeof(g_snap.msg), TR("正在启动...", "Starting..."));
   LeaveCriticalSection(&g_cs);

   g_flash_phase = FLPH_IDLE;
   g_flash_force = 0;
   g_qhead = g_qtail = 0;

   g_run = 1;
   g_thread = CreateThread(NULL, 0, ui_master_thread, adapter_buf, 0, NULL);
}

void ui_master_stop(void)
{
   if (!g_cs_init || g_thread == NULL)
      return;

   g_run = 0;
   /* 若线程阻塞在等待确认等状态，投递 QUIT 促使它退出 */
   {
      ui_cmd_msg_t m;
      m.id = UICMD_QUIT;
      m.var_index = 0;
      m.value = 0;
      m.name[0] = '\0';
      ui_master_post(&m);
   }
   WaitForSingleObject(g_thread, 10000);
   CloseHandle(g_thread);
   g_thread = NULL;
}

int ui_master_alive(void)
{
   return (g_thread != NULL) ? 1 : 0;
}

void ui_master_get_snap(ui_snap_t *out)
{
   EnterCriticalSection(&g_cs);
   *out = g_snap;
   LeaveCriticalSection(&g_cs);
}

int ui_master_pull_msgs(unsigned last_seq, ui_msgitem_t *out, int max,
                        unsigned *next_seq)
{
   int n = 0, i;
   unsigned newest = last_seq;
   if (!g_cs_init)
      return 0;
   EnterCriticalSection(&g_cs);
   for (i = 0; i < MSGLOG_MAX; ++i)
   {
      const ui_msgitem_t *it = &g_msglog[i];
      if (it->seq == 0 || it->seq <= last_seq)
         continue;
      if (n < max)
         out[n++] = *it;
      if (it->seq > newest)
         newest = it->seq;
   }
   LeaveCriticalSection(&g_cs);
   *next_seq = newest;
   return n;
}

void ui_master_post(const ui_cmd_msg_t *cmd)
{
   if (!g_cs_init || cmd == NULL)
      return;
   EnterCriticalSection(&g_cs);
   {
      int next = (g_qtail + 1) % UICMD_QUEUE_MAX;
      if (next != g_qhead) /* 队列满则丢弃最旧未处理的由 head 顶替：简单起见直接丢弃新命令 */
      {
         g_queue[g_qtail] = *cmd;
         g_qtail = next;
      }
   }
   LeaveCriticalSection(&g_cs);
}

/* 从站模式偏好：界面启动时用保存的配置调用，启动序列据此决定是否激活 DC */
void ui_master_set_mode_pref(int dc)
{
   if (dc >= 0)
      dc = dc ? 1 : 0;   /* 归一化为 1/0/-1 */
   if (!g_cs_init)
   {
      g_mode_pref = dc;
      return;
   }
   EnterCriticalSection(&g_cs);
   g_mode_pref = dc;
   LeaveCriticalSection(&g_cs);
}

int ui_master_get_mode_pref(void)
{
   int v;

   if (!g_cs_init)
      return g_mode_pref;
   EnterCriticalSection(&g_cs);
   v = g_mode_pref;
   LeaveCriticalSection(&g_cs);
   return v;
}

/* 供 ui_app 调用的一次性初始化 */
void ui_master_init(void);
void ui_master_init(void)
{
   if (g_cs_init)
      return;
   InitializeCriticalSection(&g_cs);
   memset(&g_snap, 0, sizeof(g_snap));
   g_snap.mstate = MST_IDLE;
   snprintf(g_snap.msg, sizeof(g_snap.msg),
            TR("未连接：点击「扫描新的从站」或「选择网卡」建立连接",
               "Not connected: click \"Scan New Slaves\" or \"Select NIC\" to connect"));
   memset(g_msglog, 0, sizeof(g_msglog));
   g_msglog_head = 0;
   g_msg_seq = 1;
   g_msglog[0].seq = 1;
   snprintf(g_msglog[0].text, sizeof(g_msglog[0].text), "%s", g_snap.msg);
   g_msglog_head = 1;
   g_cs_init = 1;
}
