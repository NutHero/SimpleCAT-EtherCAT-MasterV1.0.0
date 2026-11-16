/*
 * ui_master.h - 图形界面版主站后台线程
 *
 * 架构：
 *   - 后台线程（ui_master_thread）执行 EtherCAT 周期交换（1ms）、
 *     从站状态监视/自动恢复、完整重扫、EEPROM 烧录流程；
 *   - UI 线程通过 ui_master_get_snap() 读取互斥锁保护的快照
 *     （从站状态、变量值、WKC、消息、烧录进度），绝不直接访问
 *     SOEM 上下文，保证周期通信的实时性；
 *   - UI 线程通过 ui_master_post() 提交命令（写变量、DC/FreeRUN
 *     切换、重扫、烧录、强制烧录确认、退出）。
 */
#ifndef _ui_master_
#define _ui_master_

#ifdef __cplusplus
extern "C" {
#endif

#include "master_core.h"

/* 主站线程状态 */
typedef enum
{
   MST_IDLE = 0,      /* 未选择网卡，未启动 */
   MST_STARTING,      /* 正在执行启动序列（初始化/扫描/映射/切 OP） */
   MST_RUNNING,       /* 周期通信运行中（含自动恢复） */
   MST_STARTFAIL      /* 启动失败（可重试 / 烧录 ESI 后重试） */
} ui_mst_state_t;

/* CiA402（IEC 61800-5-2）驱动协议：状态机状态（由状态字 0x6041 解码） */
enum
{
   C402_ST_NONE = -1,      /* 无状态字对象 / 暂无法解码 */
   C402_ST_NOTREADY = 0,   /* 未就绪 Not ready to switch on */
   C402_ST_SODIS,          /* 未使能 Switch on disabled */
   C402_ST_RTSO,           /* 上电就绪 Ready to switch on */
   C402_ST_SON,            /* 已开通 Switched on */
   C402_ST_OPEN,           /* 运行使能 Operation enabled */
   C402_ST_FAULT,          /* 故障 Fault */
   C402_ST_FREA,           /* 故障响应 Fault reaction active */
   C402_ST_QSTOP           /* 快速停止 Quick stop active */
};

/* CiA402 控制动作（UICMD_CIA402 命令的 var_index 字段） */
enum
{
   C402_ACT_RESET = 1,      /* 故障复位：控制字 0x80 脉冲后回 0 */
   C402_ACT_DISVOLT = 2,    /* 禁止电压：控制字 0x0000 */
   C402_ACT_QSTOP = 3,      /* 快速停止：控制字 0x0002 */
   C402_ACT_SHUTDOWN = 4,   /* 上电就绪：控制字 0x0006 */
   C402_ACT_SWITCHON = 5,   /* 开通：控制字 0x0007 */
   C402_ACT_ENABLE = 6,     /* 运行使能：控制字 0x000F */
   C402_ACT_DISABLE = 7,    /* 关使能：控制字 0x0007 */
   C402_ACT_AUTOENABLE = 8, /* 一键使能：0x06 -> 0x07 -> 0x0F 序列 */
   C402_ACT_MODE = 9        /* 设置运行模式：value = 模式码（1/3/8/9...） */
};

/* 单个从站的界面快照 */
typedef struct
{
   char name[TC3_NAME_LEN];  /* 从站名称 */
   uint16 state;             /* 原始 AL 状态字 */
   char label[24];           /* TwinCAT 风格状态标签，如 OP / SAFE-OP+ERR */
   int islost;               /* 1 = 失联 */
   uint16 alstatus;          /* AL 错误码 */
   uint16 linkstat;          /* DL status（bit0..3 = 端口 0..3 链路） */
   int hasdc;                /* 支持 DC */
   int c402;                 /* 1 = 过程数据包含 CiA402 驱动对象 (0x6040/0x6041) */
   uint16 c402_sw;           /* CiA402 状态字（0x6041）当前值 */
   int c402_state;           /* C402_ST_*：解码后的驱动状态机状态 */
   int c402_mode;            /* 运行模式显示值（0x6061），-1 = 无该对象 */
} ui_slave_info_t;

/* 界面快照（ui_master_get_snap 整体拷贝，UI 端只读） */
typedef struct
{
   ui_mst_state_t mstate;         /* 主站线程状态 */
   char adapter[256];             /* 当前使用的网卡设备路径 */
   char msg[160];                 /* 最新状态消息 */
   char esi_warn[192];            /* xml/ 与从站身份不匹配警告（空 = 无） */

   int slavecount;                /* 扫描到的从站数量 */
   ui_slave_info_t slave[MASTER_MAXSLAVE + 1];

   tc3_varlist_t vl;              /* PDO 变量表（含 IOmap 偏移） */
   long long oval[TC3_MAX_VARS];  /* 输出变量当前值（按 vl.var 下标） */
   long long ival[TC3_MAX_VARS];  /* 输入变量当前值（按 vl.var 下标） */

   int wkc, expected;             /* 最近一次工作计数器 / 期望值 */
   int rt_us;                     /* 最近一次往返时间（微秒） */
   int cycle_us;                  /* 实测主循环平均周期（微秒，200ms 窗口），0 = 未运行 */
   int cycle_min_us;              /* 窗口内最小周期（微秒） */
   int cycle_max_us;              /* 窗口内最大周期（微秒），与 min 组成抖动范围 */
   long long dctime;              /* 分布式时钟当前时间戳（ns） */
   int dc_active;                 /* 1 = DC 同步已激活，0 = FreeRUN */

   /* EEPROM 烧录状态 */
   int flash_phase;               /* 0 = 空闲，1 = 构建镜像/烧录中，2 = 等待身份不符确认 */
   int flash_done, flash_total;   /* 烧录进度（写+校验步数） */
   int flash_slave;               /* 当前烧录目标从站序号（1 起） */
   char flash_file[64];           /* 正在烧录的 xml 文件名 */
   char flash_mismatch_msg[200];  /* 身份不符详情（flash_phase==2 时有效） */
   int flash_result;              /* 0 = 无，1 = 成功，-1 = 失败 */
   char flash_err[200];           /* 失败原因 */
   int flash_seq;                 /* 每次烧录结束 +1（UI 据此弹结果提示） */
} ui_snap_t;

/* UI -> 主站线程命令 */
typedef enum
{
   UICMD_NONE = 0,
   UICMD_QUIT,          /* 请求停止主站并退出线程 */
   UICMD_WRITE_VAR,     /* 写输出变量：var_index + value */
   UICMD_MODE_DC,       /* 激活 DC 同步 */
   UICMD_MODE_FREERUN,  /* 关闭 DC 同步（FreeRUN） */
   UICMD_RESCAN,        /* RUNNING: 完整重扫总线；STARTFAIL: 重试启动 */
   UICMD_FLASH_FILE,    /* 烧录指定的 xml（name）到 slave 号从站的 EEPROM */
   UICMD_FLASH_YES,     /* 身份不符：强制烧录 */
   UICMD_FLASH_NO,      /* 身份不符：取消烧录 */
   UICMD_CIA402         /* CiA402 驱动控制：slave = 目标从站（1 起），
                           var_index = C402_ACT_* 动作码，
                           value = 附加参数（C402_ACT_MODE 的模式码） */
} ui_cmd_id_t;

typedef struct
{
   ui_cmd_id_t id;
   int var_index;   /* UICMD_WRITE_VAR：变量表下标 */
   long value;      /* UICMD_WRITE_VAR：写入值 */
   int slave;       /* UICMD_FLASH_FILE / UICMD_CIA402：目标从站序号（1 起，0/负数按 1 处理） */
   char name[64];   /* UICMD_FLASH_FILE：xml 文件名 */
} ui_cmd_msg_t;

/** 供 ui_app 调用的一次性初始化（进程内只需调用一次） */
void ui_master_init(void);

/** 启动主站后台线程并连接指定网卡（重复调用会先停止旧线程） */
void ui_master_start(const char *adapter);

/** 请求停止主站并等待线程退出（从站回 INIT，关闭网络） */
void ui_master_stop(void);

/** 主站线程是否在运行（线程句柄有效） */
int ui_master_alive(void);

/** 拷贝一份当前快照（线程安全） */
void ui_master_get_snap(ui_snap_t *out);

/* 主站消息日志项（seq 全局递增，UI 按 seq 增量拉取，避免漏掉快速连续的消息） */
typedef struct
{
   unsigned seq;       /* 消息序号，从 1 开始 */
   char text[160];     /* 消息内容 */
} ui_msgitem_t;

/** 取 seq > last_seq 的所有消息（最多 max 条写入 out）。
 *  返回条数；*next_seq 返回新的 last_seq（调用方保存供下次拉取） */
int ui_master_pull_msgs(unsigned last_seq, ui_msgitem_t *out, int max,
                        unsigned *next_seq);

/** 提交一条命令到主站线程（线程安全） */
void ui_master_post(const ui_cmd_msg_t *cmd);

/** 设置从站模式偏好（下次启动序列使用）：1=DC, 0=FreeRUN, -1=不指定（按从站能力自动）。
 *  界面把它写入配置文件，实现"记住从站模式"。 */
void ui_master_set_mode_pref(int dc);

/** 读取当前的从站模式偏好（-1 = 未指定） */
int ui_master_get_mode_pref(void);

#ifdef __cplusplus
}
#endif

#endif /* _ui_master_ */
