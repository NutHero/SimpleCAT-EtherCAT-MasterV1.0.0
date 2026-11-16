/*
 * master_core.c - SimpleCAT 主站核心公共模块实现
 *
 * 从 simple_ng.c 抽取的主站核心逻辑，控制台版与图形界面版共用。
 * 逻辑与原实现保持一致，仅把控制台专属内容（printf 提示、scanf 选卡）
 * 留在调用方或改为可复用形式。
 */

#include "master_core.h"
#include "esi_flash.h"
#include "ui_lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================ 主站生命周期 ============================ */

/**
 * 初始化现场总线（Fieldbus）结构体
 * 此函数对 Fieldbus 结构体的所有成员进行清零和初始设置，为后续 EtherCAT 通信做好准备
 *
 * @param[out] fieldbus 指向要初始化的现场总线结构体的指针
 * @param[in]  iface    网络接口名称（如 "eth0"），此字符串将被保存在结构体中
 */
void
fieldbus_initialize(Fieldbus *fieldbus, char *iface)
{
   /* Let's start by 0-filling `fieldbus` to avoid surprises */
   memset(fieldbus, 0, sizeof(*fieldbus));

   fieldbus->iface = iface;
   fieldbus->group = 0;
   fieldbus->roundtrip_time = 0;
}

/**
 * 执行一次完整的过程数据交换（发送+接收），并测量往返时间
 *
 * @param[in] fieldbus 现场总线结构体指针
 * @return 工作计数器（Working Counter），指示成功交换的从站数量
 */
int
fieldbus_roundtrip(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_timet start, end, diff;
   int wkc;

   context = &fieldbus->context;

   start = osal_current_time();
   ecx_send_processdata(context); //发送从站数据
   wkc = ecx_receive_processdata(context, EC_TIMEOUTRET); //接收从站数据
   end = osal_current_time();
   osal_time_diff(&start, &end, &diff);//计算发送到接收整个过程需要的时间

   fieldbus->roundtrip_time = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);

   return wkc;
}

/**
 * 启动 EtherCAT 现场总线通信，完成从站初始化、映射和状态切换全过程
 *
 * @param[in] fieldbus 现场总线结构体指针
 * @return 成功启动并进入运行状态返回 TRUE，否则返回 FALSE
 */
boolean
fieldbus_start(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   ec_slavet *slave;
   int i;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

   //初始化 SOEM 主站，建立网络通信
   printf("Initializing SOEM on '%s'... ", fieldbus->iface);
   if (!ecx_init(context, fieldbus->iface))
   {
      printf("no socket connection\n");
      return FALSE;
   }
   printf("done\n");

   //扫描并自动配置从站
   printf("Finding autoconfig slaves... ");
   if (ecx_config_init(context) <= 0)
   {
      printf("no slaves found\n");
      return FALSE;
   }
   printf("%d slaves found\n", context->slavecount);

   //配置过程数据映射（PDO 映射）
   printf("Sequential mapping of I/O... ");
   ecx_config_map_group(context, fieldbus->map, fieldbus->group);
   printf("mapped %dO+%dI bytes from %d segments",
          grp->Obytes, grp->Ibytes, grp->nsegments);
   if (grp->nsegments > 1)
   {
      /* Show how slaves are distributed */
      for (i = 0; i < grp->nsegments; ++i)// 显示从站如何分布在多个段中（适用于多从站网络）
      {
         printf("%s%d", i == 0 ? " (" : "+", grp->IOsegment[i]);
      }
      printf(" slaves)");
   }
   printf("\n");

   //TwinCAT3 风格扫描显示：从站名称 + SM/FMMU + 所有 PDO/PDO 变量
   //变量名优先取自 xml/ 目录下的 ESI 设备描述文件（按厂商ID+产品代码匹配），
   //未匹配到时通过 CoE SDO 读取实际 PDO 映射
   tc3_scan_bus(context, "xml");

   //配置分布式时钟（DC），实现精确同步
   printf("Configuring distributed clock... ");
   ecx_configdc(context);
   printf("done\n");

   //等待所有从站进入安全运行状态（SAFE-OP）
   printf("Waiting for all slaves in safe operational... ");
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   printf("done\n");

   //发送一次测试交换，激活从站输出通道
   printf("Send a roundtrip to make outputs in slaves happy... ");
   fieldbus_roundtrip(fieldbus);
   printf("done\n");

   //尝试切换所有从站到运行状态（OP）
   printf("Setting operational state..");

   /* Act on slave 0 (a virtual slave used for broadcasting) */
   // 操作从站 0（用于广播的虚拟从站）
   slave = context->slavelist;
   slave->state = EC_STATE_OPERATIONAL;// 设置目标状态
   ecx_writestate(context, 0); // 发送状态切换命令
   /* Poll the result ten times before giving up */
   for (i = 0; i < 10; ++i)// 轮询 10 次检查状态切换结果
   {
      printf(".");
      fieldbus_roundtrip(fieldbus); // 发送过程数据保持通信
      ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
      if (slave->state == EC_STATE_OPERATIONAL)
      {
         printf(" all slaves are now operational\n");
         return TRUE;// 成功进入运行状态
      }
   }

   //状态切换失败处理，打印详细错误信息
   printf(" failed,");
   ecx_readstate(context);// 刷新所有从站状态
   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->state != EC_STATE_OPERATIONAL)
      {
          // 打印未进入 OP 状态的从站详细信息
         printf(" slave %d is 0x%04X (AL-status=0x%04X %s)",
                i, slave->state, slave->ALstatuscode,
                ec_ALstatuscode2string(slave->ALstatuscode));
      }
   }
   printf("\n");

   return FALSE;// 启动失败
}

/**
 * 安全停止现场总线（Fieldbus）通信，并清理资源
 * 此函数执行 EtherCAT 网络的优雅关闭流程，将所有从站切换回初始化状态，并释放网络资源
 *
 * @param[in] fieldbus 指向要停止的现场总线结构体的指针
 */
void
fieldbus_stop(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_slavet *slave;

   context = &fieldbus->context;
   /* Act on slave 0 (a virtual slave used for broadcasting) */
   slave = context->slavelist;

   printf("Requesting init state on all slaves... ");
   slave->state = EC_STATE_INIT;
   ecx_writestate(context, 0);
   printf("done\n");

   printf("Close socket... ");
   ecx_close(context);
   printf("done\n");
}

/* ============================ 从站状态监视与自动恢复 ============================ */

static unsigned long long g_lost_since[EC_MAXSLAVE + 1]; /* 每从站首次异常时刻，0=正常 */
static uint16 g_linkstat[EC_MAXSLAVE + 1];               /* 每从站 DL status（端口链路） */
static unsigned long long g_last_link_read;
static uint16 g_prev_mon_state[EC_MAXSLAVE + 1];         /* 上次监视到的从站状态（消息只在变化时发） */
static unsigned long long g_last_safeop_msg[EC_MAXSLAVE + 1]; /* 每从站上次发 SAFE-OP 消息的时刻（冷却用） */
static int g_need_reinit;                                /* 需要完整重扫总线 */

uint16 master_linkstat(int i)
{
   if (i < 0 || i > EC_MAXSLAVE)
      return 0;
   return g_linkstat[i];
}

int master_need_reinit(void)
{
   return g_need_reinit;
}

void master_clear_reinit(void)
{
   g_need_reinit = 0;
}

/* 非破坏性探测当前物理在网的从站数量（广播读 Type 寄存器，不改变任何配置） */
int
probe_bus_slaves(ecx_contextt *context)
{
   uint16 w = 0;
   return ecx_BRD(&context->port, 0x0000, ECT_REG_TYPE, sizeof(w), &w,
                  EC_TIMEOUTSAFE);
}

/* 把 AL 状态字转换成 TwinCAT 风格的状态名，如 OP / SAFE-OP / PRE-OP / INIT */
const char *
slave_state_str(uint16 state)
{
   switch (state & 0x0F)
   {
      case EC_STATE_INIT:        return "INIT";
      case EC_STATE_PRE_OP:      return "PRE-OP";
      case EC_STATE_BOOT:        return "BOOT";
      case EC_STATE_SAFE_OP:     return "SAFE-OP";
      case EC_STATE_OPERATIONAL: return "OP";
      case EC_STATE_NONE:        return "NONE";
      default:                   return "?";
   }
}

/* 生成完整状态标签，带错误标志，如 "SAFE-OP+ERR" */
void
slave_state_label(uint16 state, char *buf, size_t sz)
{
   if (state & EC_STATE_ERROR)
      snprintf(buf, sz, "%s+ERR", slave_state_str(state));
   else
      snprintf(buf, sz, "%s", slave_state_str(state));
}

/*
 * 从站状态监视 + 自动恢复（每 MON_REFRESH_MS 执行一次，由主循环调用）
 *
 * 关键点：从站断电再上电后会重新回到 INIT，且站地址回到 EEPROM 中的值（通常为 0），
 * 此时主站缓存的 configadr 已经无效。必须先执行 ecx_recover_slave() 通过
 * 自增寻址重新写入站地址，才可能用 ecx_reconfig_slave() 恢复 SM/FMMU 配置。
 * 旧代码直接调用 reconfig（用的是失效的 configadr），所以从站重新上电后永远检测不回来。
 */
void
fieldbus_monitor(Fieldbus *fieldbus, int wkc, int expected,
                 char *msg, size_t msgsz)
{
   ecx_contextt *context = &fieldbus->context;
   unsigned long long now = GetTickCount64();
   static unsigned long long last_mon = 0;
   int bad, i;

   /* 先清空输出缓冲：本函数可能因节流提前返回且什么都不写，
      调用方的缓冲是循环内复用的栈内存，不清空会把上一条旧消息
      一次又一次重复上报（表现为某条消息每 2 秒重复入环刷屏） */
   if (msg != NULL && msgsz > 0)
      msg[0] = '\0';

   bad = (wkc < expected);
   /* 固定按 MON_REFRESH_MS 节流：恢复过程中的寄存器读写有毫秒级耗时，
      不能每个通信周期都做，否则会拖慢 1ms 的过程数据周期 */
   if (last_mon != 0 && (now - last_mon) < MON_REFRESH_MS)
      return;
   last_mon = now;

   ecx_readstate(context); /* 刷新所有从站的 AL 状态 */

   /* 每秒读一次 DL status，得到各端口链路（TwinCAT 的 Link 指示） */
   if (now - g_last_link_read >= MON_LINK_MS)
   {
      g_last_link_read = now;
      for (i = 1; i <= context->slavecount; ++i)
      {
         uint16 v = 0;
         if (ecx_FPRD(&context->port, context->slavelist[i].configadr,
                      ECT_REG_DLSTAT, sizeof(v), &v, EC_TIMEOUTRET) > 0)
            g_linkstat[i] = etohs(v);
         else
            g_linkstat[i] = 0;
      }
   }

   for (i = 1; i <= context->slavecount; ++i)
   {
      ec_slavet *sl = &context->slavelist[i];
      uint16 st = sl->state;
      uint16 pst = g_prev_mon_state[i];   /* 上次监视到的状态 */

      g_prev_mon_state[i] = st;           /* 本轮状态记为"上次"，供下轮比较 */

      /* --- 正常 --- */
      if (st == EC_STATE_OPERATIONAL)
      {
         if (g_lost_since[i] != 0)
         {
            g_lost_since[i] = 0;
            sl->islost = FALSE;
            snprintf(msg, msgsz, TR("从站 %d (%s) 已恢复 OP",
                                    "Slave %d (%s) back to OP"), i, sl->name);
         }
         continue;
      }

      if (g_lost_since[i] == 0)
         g_lost_since[i] = now;

      /* --- 从站报应用层错误：发 ACK 清除（从站在网且有进展，清失联计时） --- */
      if (st & EC_STATE_ERROR)
      {
         g_lost_since[i] = 0;
         sl->state = (uint16)((st & 0x0F) | EC_STATE_ACK);
         ecx_writestate(context, (uint16)i);
         snprintf(msg, msgsz, TR("从站 %d 报错 (0x%04X %s)，已发送 ACK",
                                 "Slave %d error (0x%04X %s), ACK sent"), i, st,
                  ec_ALstatuscode2string(sl->ALstatuscode));
         continue;
      }

      /* --- SAFE-OP：直接请求切到 OP（从站在网且有进展，清失联计时） ---
       * 消息受双重抑制：a) 仅状态变化时发；b) 每从站最长 60s 一条冷却。
       * 有的从站会在 SAFE-OP/OP 间来回振荡，若无冷却每 200ms 都会重新
       * 满足"状态变化"条件把日志刷屏；停留期间每轮照常静默重试 writestate */
      if (st == EC_STATE_SAFE_OP)
      {
         g_lost_since[i] = 0;
         sl->state = EC_STATE_OPERATIONAL;
         ecx_writestate(context, (uint16)i);
         if (pst != EC_STATE_SAFE_OP &&
             now - g_last_safeop_msg[i] > MON_SAFEOP_MSG_MS)
         {
            g_last_safeop_msg[i] = now;
            snprintf(msg, msgsz,
                     TR("从站 %d 在 SAFE-OP，请求切换到 OP (AL错误码 0x%04X %s)",
                        "Slave %d in SAFE-OP, requesting OP (AL error 0x%04X %s)"),
                     i, sl->ALstatuscode,
                     ec_ALstatuscode2string(sl->ALstatuscode));
         }
         continue;
      }

      /* --- INIT / PRE-OP / BOOT / 失联：先恢复站地址，再重配 SM/FMMU --- */
      if (ecx_recover_slave(context, (uint16)i, EC_TIMEOUTRET3) <= 0)
      {
         /* 从站不在网上（断电 / 断线）：保持失联标记，等待重新上电 */
         if (!sl->islost)
         {
            sl->islost = TRUE;
            snprintf(msg, msgsz, TR("从站 %d 失联，等待重新上电...",
                                    "Slave %d lost, waiting for power cycle..."), i);
         }
         continue;
      }

      if (sl->islost)
      {
         sl->islost = FALSE;
         snprintf(msg, msgsz, TR("从站 %d 重新上线，正在重新配置...",
                                 "Slave %d back online, reconfiguring..."), i);
      }

      /* 站地址已恢复（从站处于 INIT），重新写入 SM / FMMU 并推到 SAFE-OP */
      if (ecx_reconfig_slave(context, (uint16)i, EC_TIMEOUTRET3) == EC_STATE_SAFE_OP)
      {
         g_lost_since[i] = 0; /* 重配成功：从站已在网且配置恢复，不再计入失联 */
         sl->state = EC_STATE_OPERATIONAL;
         ecx_writestate(context, (uint16)i);
         ecx_statecheck(context, (uint16)i, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
         if (sl->hasdc) /* 断电后 DC 参数丢失，重新激活同步 */
            ecx_dcsync0(context, (uint16)i, TRUE, 1000000, 0);
         snprintf(msg, msgsz, TR("从站 %d 重新配置完成，已恢复 OP",
                                 "Slave %d reconfigured, back to OP"), i);
      }
      else
      {
         snprintf(msg, msgsz, TR("从站 %d 重新配置中...",
                                 "Slave %d reconfiguring..."), i);
      }
   }

   /* --- 长时间无法恢复：请求完整重扫（应对换从站、增减从站等拓扑变化） ---
    * 关键保护：总线上一个从站都没有时绝不触发重扫！
    * ecx_config_init() 内部第一步就是 ecx_init_context()，会把 slavecount
    * 清零、slavelist/grouplist 全部清空。若在空总线上执行，缓存的
    * configadr、SM/FMMU、EEPROM 身份信息将全部丢失，从站重新插上后
    * 恢复路径彻底失效（表现为"长时间拔掉再插上检测不到"）。 */
   if (bad)
   {
      for (i = 1; i <= context->slavecount; ++i)
      {
         if (g_lost_since[i] != 0 && (now - g_lost_since[i]) > MON_REINIT_MS)
         {
            if (probe_bus_slaves(context) > 0) /* 总线上确有从站才允许重扫 */
               g_need_reinit = 1;
            break;
         }
      }
   }
}

/*
 * 完整重新扫描总线：重新枚举从站 -> 重新映射 PDO -> 重建变量表 -> 回到 OP
 * 用于从站长时间失联、或拓扑发生变化（更换/增减从站）后的兜底恢复。
 */
boolean
fieldbus_full_reinit(Fieldbus *fieldbus, tc3_varlist_t *vl,
                     char *msg, size_t msgsz)
{
   /* ecx_config_init() 会先 ecx_init_context() 清空整个上下文，
      因此重扫前必须备份，失败时回滚，否则恢复路径全部失效 */
   static ec_slavet slaves_bak[EC_MAXSLAVE + 1];
   static ec_groupt groups_bak[EC_MAXGROUP];
   ecx_contextt *context = &fieldbus->context;
   ec_groupt *grp = context->grouplist + fieldbus->group;
   ec_slavet *slave = context->slavelist;
   int saved_slavecount;
   int i, n;

   printf("\n[总线恢复] 从站长时间未恢复，执行完整重新扫描 ...\n");

   /* 0) 备份上下文 */
   saved_slavecount = context->slavecount;
   memcpy(slaves_bak, context->slavelist, sizeof(slaves_bak));
   memcpy(groups_bak, context->grouplist, sizeof(groups_bak));

   /* 1) 全部从站回 INIT 并重新枚举（重新读取 EEPROM、重写站地址） */
   slave->state = EC_STATE_INIT;
   ecx_writestate(context, 0);

   n = ecx_config_init(context);
   if (n <= 0)
   {
      printf("[总线恢复] 重新扫描失败：总线上没有从站，已回滚原配置\n");
      /* 回滚：恢复 slavecount / slavelist / grouplist，
         从站重新上线后 recover 路径仍然可用 */
      context->slavecount = saved_slavecount;
      memcpy(context->slavelist, slaves_bak, sizeof(slaves_bak));
      memcpy(context->grouplist, groups_bak, sizeof(groups_bak));
      snprintf(msg, msgsz, TR("重新扫描失败：总线上没有从站，等待上电",
                              "Rescan failed: no slave on the bus, waiting for power-up"));
      for (i = 0; i <= EC_MAXSLAVE; ++i)
         g_lost_since[i] = GetTickCount64();
      return FALSE;
   }
   printf("[总线恢复] 重新扫描到 %d 个从站\n", n);

   /* 2) 重新映射过程数据（复用 fieldbus->map 缓冲区） */
   ecx_config_map_group(context, fieldbus->map, fieldbus->group);
   printf("[总线恢复] PDO 重新映射：%dO + %dI 字节\n", grp->Obytes, grp->Ibytes);

   /* 3) DC 重新配置 */
   if (context->slavelist[0].hasdc)
      ecx_configdc(context);

   /* 4) SAFE-OP -> OP */
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   slave->state = EC_STATE_OPERATIONAL;
   ecx_writestate(context, 0);
   for (i = 0; i < 10; ++i)
   {
      fieldbus_roundtrip(fieldbus);
      ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
      if (slave->state == EC_STATE_OPERATIONAL)
         break;
   }

   /* 5) 每个支持 DC 的从站重新激活同步 */
   for (i = 1; i <= context->slavecount; ++i)
   {
      if (context->slavelist[i].hasdc)
         ecx_dcsync0(context, (uint16)i, TRUE, 1000000, 0);
   }

   /* 6) 拓扑可能变化，重建 PDO 变量表 */
   tc3_build_varlist(context, "xml", vl);

   /* 7) 失联计时清零 */
   for (i = 0; i <= EC_MAXSLAVE; ++i)
   {
      g_lost_since[i] = 0;
      g_prev_mon_state[i] = 0;      /* 重扫后允许重新报告状态变化消息 */
      g_last_safeop_msg[i] = 0;     /* 重扫后冷却清零 */
   }

   printf("[总线恢复] %s\n\n",
          slave->state == EC_STATE_OPERATIONAL ? "完成，总线已恢复 OP" : "从站仍未能进入 OP");
   snprintf(msg, msgsz, slave->state == EC_STATE_OPERATIONAL ?
            TR("总线已重新扫描并恢复 OP", "Bus rescanned and back to OP") :
            TR("重新扫描后仍有从站不在 OP", "Some slaves still not in OP after rescan"));

   return (slave->state == EC_STATE_OPERATIONAL) ? TRUE : FALSE;
}

/* ============================ PDO 变量读写辅助 ============================ */

/* 按数据类型判断符号性（INT/SINT/DINT 有符号，其余无符号） */
boolean
var_is_signed(const tc3_variable_t *v)
{
   return (boolean)(strcmp(v->type, "INT") == 0 ||
                    strcmp(v->type, "SINT") == 0 ||
                    strcmp(v->type, "DINT") == 0);
}

/* 从 IOmap 缓冲读取变量当前值 */
long long
read_var_value(const tc3_variable_t *v, const uint8 *base)
{
   switch (v->bitlen)
   {
      case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      {
         /* 子字节变量（BOOL 等）：按位偏移取出 bitlen 位 */
         uint8 mask = (uint8)((0xFFu >> (8 - v->bitlen)) << v->bitoff);
         return (long long)((base[v->offset] & mask) >> v->bitoff);
      }
      case 8:
         return var_is_signed(v) ? (long long)(int8_t)base[v->offset]
                                 : (long long)base[v->offset];
      case 16:
      {
         uint16 u = 0;
         memcpy(&u, base + v->offset, 2);
         return var_is_signed(v) ? (long long)(int16_t)u : (long long)u;
      }
      case 32:
      {
         uint32 u = 0;
         memcpy(&u, base + v->offset, 4);
         return var_is_signed(v) ? (long long)(int32_t)u : (long long)u;
      }
      default:
         return 0;
   }
}

/* 在变量表中按名称查找（不区分大小写，dir: 0=输出 1=输入） */
int
find_var_by_name(const tc3_varlist_t *vl, const char *name, int dir)
{
   int i;
   for (i = 0; i < vl->count; i++)
   {
      if (vl->var[i].dir == dir && _stricmp(vl->var[i].name, name) == 0)
         return i;
   }
   return -1;
}

/* 在变量表中按 "索引:子索引" 查找，如 0x7010:03 */
int
find_var_by_index(const tc3_varlist_t *vl, uint16 idx, uint8 sub, int dir)
{
   int i;
   for (i = 0; i < vl->count; i++)
   {
      if (vl->var[i].dir == dir && vl->var[i].index == idx && vl->var[i].sub == sub)
         return i;
   }
   return -1;
}

/* 把值写入 IOmap 输出缓冲（小端，主站下个周期自动下发到从站） */
void
write_var_value(const tc3_variable_t *v, uint8 *outputs, long value,
                char *msg, size_t msgsize)
{
   switch (v->bitlen)
   {
      case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      {
         /* 子字节变量（BOOL 等）：读-改-写所在字节，只动自己的位 */
         uint8 mask = (uint8)((0xFFu >> (8 - v->bitlen)) << v->bitoff);
         uint8 cur = outputs[v->offset];
         cur = (uint8)((cur & (uint8)(~mask)) |
                       (uint8)(((uint8)value << v->bitoff) & mask));
         memcpy(outputs + v->offset, &cur, 1);
         break;
      }
      case 8:
      {
         int8_t s = (int8_t)value;
         memcpy(outputs + v->offset, &s, 1);
         break;
      }
      case 16:
      {
         int16_t s = (int16_t)value;
         memcpy(outputs + v->offset, &s, 2);
         break;
      }
      case 32:
      {
         int32_t s = (int32_t)value;
         memcpy(outputs + v->offset, &s, 4);
         break;
      }
      default:
         snprintf(msg, msgsize, TR("不支持写入 %d 位变量",
                                   "Writing %d-bit variables not supported"), v->bitlen);
         return;
   }
   snprintf(msg, msgsize, TR("已写入 %s = %ld (输出偏移 0x%04X.%d)",
                             "Written %s = %ld (output offset 0x%04X.%d)"),
            v->name, value, v->offset, v->bitoff);
}

/* 去除字符串首尾空白（就地修改），含 \r \n（fgets 读入的换行也一并去掉） */
void
trim_inplace(char *s)
{
   char *p = s;
   size_t len;

   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
   len = strlen(s);
   while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                      s[len - 1] == '\r' || s[len - 1] == '\n'))
      s[--len] = '\0';
}

/* ============================ 网卡枚举 ============================ */

/**
 * 枚举系统上所有可用的网络适配器（网卡）
 * 使用 SOEM 的 ec_find_adapters() 枚举系统上所有可用的网卡接口。
 * 调用方使用完毕后须调用 ec_free_adapters() 释放链表。
 */
ec_adaptert *
master_list_adapters(int *count)
{
   ec_adaptert *head, *adapter;
   int n = 0;

   head = adapter = ec_find_adapters();
   while (adapter != NULL)
   {
      n++;
      adapter = adapter->next;
   }
   if (count != NULL)
      *count = n;
   return head;
}

/* ============================ 电机控制占位 ============================ */

// 函数功能：发送16位有符号数到outputs
void send_int16_to_outputs(uint8* outputs, int start_index, int16 value)
{
    outputs[start_index] = (uint8)(value & 0xFF);
    outputs[start_index + 1] = (uint8)((value >> 8) & 0xFF);
}

// 函数功能：发送32位有符号数到outputs
void send_int32_to_outputs(uint8* outputs, int start_index, int32 value)
{
    outputs[start_index] = (uint8)(value & 0xFF);
    outputs[start_index + 1] = (uint8)((value >> 8) & 0xFF);
    outputs[start_index + 2] = (uint8)((value >> 16) & 0xFF);
    outputs[start_index + 3] = (uint8)((value >> 24) & 0xFF);
}

// 函数功能：设置电机速度
void set_motor_speed(uint8 num, int16 value)
{

}

// 函数功能：设置电机角度
void set_motor_angle(uint8 num, int16 value)
{

}
