/*
 * master_core.h - SimpleCAT 主站核心公共模块
 *
 * 从 simple_ng.c 抽取的主站核心逻辑，供控制台版（simple_ng.c 的
 * main()）与图形界面版（ui/ui_master.c 的后台线程）共用：
 *   - Fieldbus 主站上下文与生命周期（init / start / stop / roundtrip）
 *   - 从站状态监视 + 自动恢复 + 完整重扫（fieldbus_monitor / full_reinit）
 *   - PDO 变量表的读写辅助（read_var_value / write_var_value / find_var_*）
 *   - 网卡枚举（master_list_adapters）
 *
 * 控制台交互（命令解析、VT 界面绘制）不属于核心逻辑，仍保留在
 * simple_ng.c 中；图形界面相关代码在 ui/ 目录。
 */
#ifndef _master_core_
#define _master_core_

#ifdef __cplusplus
extern "C" {
#endif

#include "soem.h"
#include "tc3_scan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ============================ 主站控制上下文 ============================ */

/**
 * 现场总线（Fieldbus）主站控制上下文结构体
 * 此结构体封装了 SOEM 主站的核心状态和数据，用于管理 EtherCAT 网络通信
 */
typedef struct
{
   ecx_contextt context; /**< SOEM 库的主站上下文，包含从站列表、组配置等核心状态 */
   char *iface;          /**< 网络接口名称字符串指针，例如 "eth0" 或 Windows 设备路径 */
   uint8 group;          /**< 过程数据组号，通常为 0（默认组），SOEM 支持多组数据交换 */
   int roundtrip_time;   /**< 上一次过程数据交换的往返时间（微秒），用于性能监控 */
   uint8 map[4096];      /**< 过程数据映射缓冲区，最大支持 4096 字节的映射空间 */
} Fieldbus;

/** 初始化 Fieldbus 结构体（清零 + 保存接口名） */
void fieldbus_initialize(Fieldbus *fieldbus, char *iface);

/**
 * 执行一次完整的过程数据交换（发送+接收），并测量往返时间
 * @return 工作计数器（Working Counter），指示成功交换的从站数量
 */
int fieldbus_roundtrip(Fieldbus *fieldbus);

/**
 * 启动 EtherCAT 现场总线通信：初始化 -> 扫描配置 -> PDO 映射 ->
 * TwinCAT 风格扫描显示 -> DC -> SAFE-OP -> OP
 * @return 成功进入运行状态返回 TRUE，否则返回 FALSE
 */
boolean fieldbus_start(Fieldbus *fieldbus);

/** 安全停止现场总线通信：所有从站回 INIT 并关闭网络 */
void fieldbus_stop(Fieldbus *fieldbus);

/* ============================ 从站状态监视与自动恢复 ============================ */

#define MON_REFRESH_MS  200ULL    /* 状态轮询节流周期（ms） */
#define MON_REINIT_MS   4000ULL   /* 单从站失联超过该时长 -> 触发完整重扫（ms） */
#define MON_SAFEOP_MSG_MS 60000ULL /* SAFE-OP 请求切 OP 消息的每从站冷却时长（ms） */
#define MON_LINK_MS     1000ULL   /* 链路状态（DL status）读取周期（ms） */

/* 最大从站数（与 SOEM 一致，用于失联/链路状态数组） */
#define MASTER_MAXSLAVE EC_MAXSLAVE

/**
 * 从站状态监视 + 自动恢复（每 MON_REFRESH_MS 执行一次，由主循环调用）
 * 断电重上电后自动恢复站地址、重配 SM/FMMU、重回 OP；
 * 长时间无法恢复时置位重扫请求（master_need_reinit() 返回 1）。
 *
 * @param[in]    fieldbus 主站上下文
 * @param[in]    wkc      最近一次过程数据交换的工作计数器
 * @param[in]    expected 期望的工作计数器
 * @param[out]   msg      状态消息缓冲（如"从站 2 失联，等待重新上电..."）
 * @param[in]    msgsz    msg 大小
 */
void fieldbus_monitor(Fieldbus *fieldbus, int wkc, int expected,
                      char *msg, size_t msgsz);

/**
 * 完整重新扫描总线：重新枚举从站 -> 重新映射 PDO -> 重建变量表 -> 回到 OP
 * 用于从站长时间失联、或拓扑发生变化（更换/增减从站）后的兜底恢复。
 */
boolean fieldbus_full_reinit(Fieldbus *fieldbus, tc3_varlist_t *vl,
                             char *msg, size_t msgsz);

/** 从站 i（1 起）最近一次读取的 DL status（bit0..3 = 端口 0..3 链路） */
uint16 master_linkstat(int i);

/** 是否有长时间无法恢复的从站等待完整重扫（1 = 需要，处理后调 master_clear_reinit） */
int master_need_reinit(void);

/** 清除完整重扫请求标志 */
void master_clear_reinit(void);

/** 把 AL 状态字转换成 TwinCAT 风格的状态名，如 OP / SAFE-OP / PRE-OP / INIT */
const char *slave_state_str(uint16 state);

/** 生成完整状态标签，带错误标志，如 "SAFE-OP+ERR" */
void slave_state_label(uint16 state, char *buf, size_t sz);

/** 非破坏性探测当前物理在网的从站数量（广播读 Type 寄存器） */
int probe_bus_slaves(ecx_contextt *context);

/* ============================ PDO 变量读写辅助 ============================ */

/** 按数据类型判断符号性（INT/SINT/DINT 有符号，其余无符号） */
boolean var_is_signed(const tc3_variable_t *v);

/** 从 IOmap 缓冲读取变量当前值（base 传 outputs 或 inputs） */
long long read_var_value(const tc3_variable_t *v, const uint8 *base);

/** 在变量表中按名称查找（不区分大小写，dir: 0=输出 1=输入），找不到返回 -1 */
int find_var_by_name(const tc3_varlist_t *vl, const char *name, int dir);

/** 在变量表中按 "索引:子索引" 查找（如 0x7010:03），找不到返回 -1 */
int find_var_by_index(const tc3_varlist_t *vl, uint16 idx, uint8 sub, int dir);

/**
 * 把值写入 IOmap 输出缓冲（小端，主站下个周期自动下发到从站）
 * @param[out] msg  操作结果消息（成功或失败原因）
 */
void write_var_value(const tc3_variable_t *v, uint8 *outputs, long value,
                     char *msg, size_t msgsize);

/** 去除字符串首尾空白（就地修改），含 \r \n */
void trim_inplace(char *s);

/* ============================ 网卡枚举 ============================ */

/**
 * 枚举系统上所有可用的网卡（WinPcap/Npcap 接口）
 * @param[out] count 实际枚举到的网卡数量（可为 NULL）
 * @return 网卡链表头（用完须调用 ec_free_adapters 释放）；无网卡返回 NULL
 */
ec_adaptert *master_list_adapters(int *count);

/* ============================ 电机控制占位 ============================ */

void send_int16_to_outputs(uint8* outputs, int start_index, int16 value);
void send_int32_to_outputs(uint8* outputs, int start_index, int32 value);
void set_motor_speed(uint8 num, int16 value);
void set_motor_angle(uint8 num, int16 value);

#ifdef __cplusplus
}
#endif

#endif /* _master_core_ */
