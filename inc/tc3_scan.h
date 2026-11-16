/*
 * tc3_scan.h - TwinCAT3 风格的 EtherCAT 总线扫描显示模块
 *
 * 功能（对标 TwinCAT3 的 "Scan Boxes" + Process Image 视图）：
 *  1. 显示扫描到的每个从站：名称、厂商 ID / 产品代码 / 版本号、
 *     配置地址、状态、邮箱配置、DC 能力、SyncManager、FMMU。
 *  2. 显示从站的所有 RxPDO / TxPDO 及其内部变量（PDO Entry）：
 *     CoE 索引、名称、数据类型、位宽，以及变量在主站 IOmap
 *     过程映像中的位偏移。
 *  3. 变量名优先取自 xml/ 目录下的 ESI 设备描述文件（按
 *     厂商 ID + 产品代码匹配）；未匹配到时回退为通过 CoE
 *     SDO (0x1C12/0x1C13 及 PDO 映射对象) 读取实际映射。
 *
 * 用法：
 *   在 ecx_config_init() + ecx_config_map_group() 之后调用：
 *     tc3_scan_bus(context, "xml");
 */
#ifndef _tc3_scan_
#define _tc3_scan_

#ifdef __cplusplus
extern "C" {
#endif

#include "soem.h"

/* ---------------- ESI 解析用数据结构（供扫描与 EEPROM 烧录共用） ---------------- */

#define TC3_MAX_SM      8   /* 每设备最多 SyncManager 数 */
#define TC3_MAX_PDO     16  /* 每设备最多 PDO 数 */
#define TC3_MAX_ENTRY   64  /* 每个 PDO 最多变量数 */
#define TC3_NAME_LEN    64

typedef struct
{
   uint16 index;             /* CoE 对象索引，如 0x7010 */
   uint8  subindex;          /* 子索引 */
   int    bitlen;            /* 位宽 */
   char   name[TC3_NAME_LEN];/* 变量名（来自 ESI） */
   char   type[24];          /* 数据类型，如 BOOL / INT */
} tc3_entry_t;

typedef struct
{
   int    dir;               /* 0 = RxPDO(输出, SM2), 1 = TxPDO(输入, SM3) */
   int    sm;                /* 所属 SyncManager 编号 */
   uint16 index;             /* PDO 对象索引，如 0x1601 */
   char   name[TC3_NAME_LEN];
   int    entry_count;
   tc3_entry_t entry[TC3_MAX_ENTRY];
} tc3_pdo_t;

typedef struct
{
   int    start;             /* SM 起始物理地址 */
   int    size;              /* 默认长度（字节） */
   char   name[24];          /* MBoxOut / MBoxIn / Outputs / Inputs */
} tc3_sm_t;

typedef struct
{
   char    dev_name[TC3_NAME_LEN]; /* 设备类型名 */
   uint32  vendor_id;
   uint32  product_code;
   uint32  revision;
   int     sm_count;
   tc3_sm_t sm[TC3_MAX_SM];
   int     pdo_count;
   tc3_pdo_t pdo[TC3_MAX_PDO];
} tc3_device_t;

/**
 * 从指定的单个 ESI XML 文件解析设备描述（取文件中第一个 <Device>）
 * @return 0 成功；<0 失败（文件不存在 / 无 <Device>）
 */
int tc3_load_device_from_file(const char *path, tc3_device_t *dev);

/**
 * 在 xml_dir 目录下按 厂商 ID + 产品代码 搜索匹配的 ESI 设备描述
 * @param[out] matched_file 匹配到的文件名（可为 NULL）
 * @return 0 匹配成功；-1 未匹配
 */
int tc3_find_device_in_dir(const char *xml_dir, uint32 vendor,
                           uint32 product, tc3_device_t *dev,
                           char *matched_file, size_t matchsize);

/**
 * 扫描并打印所有从站的 TwinCAT3 风格详细信息
 *
 * @param[in] context SOEM 主站上下文（已完成 config_init + config_map_group）
 * @param[in] xml_dir ESI 设备描述 XML 文件所在目录（如 "xml"），可为 NULL
 * @return 成功显示的从站数量
 */
int tc3_scan_bus(ecx_contextt *context, const char *xml_dir);

/* ---------------- 实时读写用变量表 ---------------- */

#define TC3_MAX_VARS 128

typedef struct
{
   char   name[64];   /* 变量名（ESI 提供或 "0x7010:03" 索引形式） */
   uint16 index;      /* CoE 对象索引 */
   uint8  sub;        /* 子索引 */
   int    bitlen;     /* 位宽 */
   int    dir;        /* 0 = RxPDO(输出, 主站可写), 1 = TxPDO(输入, 只读) */
   uint8  slave;      /* 所属从站编号（1 起，0 = 未知/旧数据） */
   uint32 offset;     /* 在组 IOmap outputs/inputs 缓冲中的字节偏移 */
   uint8  bitoff;     /* 字节内位偏移 */
   char   type[24];   /* 数据类型（ESI 提供时，如 BOOL/USINT/INT/UINT） */
} tc3_variable_t;

typedef struct
{
   tc3_variable_t var[TC3_MAX_VARS];
   int count;
} tc3_varlist_t;

/**
 * 建立全部从站的 PDO 变量表（含每个变量在 IOmap 中的偏移）。
 * 须在 ecx_config_map_group() 之后调用。
 *
 * @param[in]  context SOEM 主站上下文
 * @param[in]  xml_dir ESI 设备描述 XML 目录（如 "xml"），可为 NULL
 * @param[out] list    输出的变量表
 * @return 变量总数（<0 表示参数错误）
 */
int tc3_build_varlist(ecx_contextt *context, const char *xml_dir,
                      tc3_varlist_t *list);

/**
 * 检查所有从站是否都能在 xml_dir 下匹配到 ESI 设备描述
 * （即"xml 文件和从站不匹配"的检测，供实时界面显示警告）
 *
 * @param[in]  context SOEM 主站上下文（已完成 config_init）
 * @param[in]  xml_dir ESI XML 目录（如 "xml"）
 * @param[out] msg     摘要信息缓冲（全匹配时写入空串，可为 NULL）
 * @param[in]  msz     msg 大小
 * @return 未匹配的从站数量（0 = 全部匹配）
 */
int tc3_esi_mismatch_summary(ecx_contextt *context, const char *xml_dir,
                             char *msg, size_t msz);

#ifdef __cplusplus
}
#endif

#endif /* _tc3_scan_ */
