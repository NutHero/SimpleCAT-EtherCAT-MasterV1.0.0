/*
 * esi_flash.h - ESI XML → SII EEPROM 烧录模块
 *
 * 功能（对标 TwinCAT3 的 "Write EEPROM"）：
 *  1. 列出 xml/ 目录下可解析的 .xml 设备描述文件；
 *  2. 取得 EEPROM 访问权（从站切 INIT + EEPROM 归主站）；
 *  3. 将选定的 ESI XML 转换为 SII 类别区镜像；
 *  4. 通过 SII 寄存器 (0x0500-0x0508) 逐字烧录到从站 EEPROM，
 *     并逐字读回校验，全程回调进度。
 *
 * 安全策略（防止烧砖）：
 *  - 重写身份区（字 0x0008-0x000D：厂商/产品/修订版本，取自 XML）和
 *    类别区（字 0x0040 起：STRINGS / GENERAL / FMMU / SYNCM /
 *    TxPDO / RxPDO + 0xFFFF 终止符）——使烧录后从站身份与新 ESI 一致；
 *  - **不碰** PDI 配置 + CRC 校验区（字 0x0000-0x0007）和序列号
 *    （字 0x000E-0x000F）：CRC 区写错 ESC 会无法启动，而本功能没有
 *    修改它的必要；身份区不参与 CRC 校验，写错也不会导致无法启动；
 *  - 烧录前校验 XML 的产品代码 / 厂商 ID 与在线从站一致，不符时返回
 *    ESI_FLASH_ERR_MISMATCH（由上层决定是否强制烧录——换装新 ESI 时
 *    身份本来就不会一致，TwinCAT 同样允许给从站写任意 ESI）；
 *  - ESC 仅在上电复位时从 EEPROM 加载配置区，烧录完成后须给从站重新上电。
 *
 * 关于 EEPROM 访问权（这是"读回全 0"的根因）：
 *  ESC 的 EEPROM 由 EtherCAT 主站与 PDI（从站 MCU）共享，寄存器 0x0500
 *  的 bit0 决定归属。从站进入 OP、PDI 固件运行后通常持有访问权，此时
 *  主站读写全部 NACK、ecx_readeepromFP 返回 0。因此烧录前必须：
 *    ① 从站切到 INIT；② 写 0x0500=2（从 PDI 强制收回）再写 0（归主站）。
 */
#ifndef _esi_flash_
#define _esi_flash_

#ifdef __cplusplus
extern "C" {
#endif

#include "soem.h"
#include "tc3_scan.h"

#define ESI_FLASH_MAX_FILES 16   /* 最多列出的 xml 文件数 */
#define ESI_FLASH_NAME_MAX  64   /* 文件名缓冲大小 */
#define ESI_FLASH_MAX_WORDS 512  /* 4Kbit EEPROM = 512 字（16bit/字） */
#define ESI_FLASH_FIXED     0x40 /* 固定区字数，类别区从字 0x0040 开始 */

#define ESI_FLASH_ID_START  0x08 /* 身份区起始字（厂商 ID） */
#define ESI_FLASH_ID_WORDS  6    /* 身份区字数：厂商2 + 产品2 + 修订版本2 */

/* build_image 返回值：XML 身份与从站不符（上层可提示后强制烧录） */
#define ESI_FLASH_ERR_MISMATCH (-10)

/* 烧录进度回调（每写若干字调用一次） */
typedef void (*esi_flash_progress_t)(void *user, int done_words, int total_words);

/**
 * 列出目录下所有可解析出 <Device> 的 .xml 文件
 *
 * @param[in]  dir        目录（如 "xml"）
 * @param[out] files      文件名数组（仅文件名，不含目录）
 * @param[in]  max_files  数组容量
 * @return 实际列出的文件数量
 */
int esi_flash_list_xml(const char *dir,
                       char files[][ESI_FLASH_NAME_MAX], int max_files);

/**
 * 进入烧录前准备：从站切 INIT + 强制 EEPROM 归主站 + 探测可读性
 *
 * 必须在 build_image / burn 之前调用（否则 EEPROM 归 PDI 时会读回全 0）。
 *
 * @param[in]  context SOEM 上下文
 * @param[in]  slave   目标从站编号（1 起）
 * @param[out] err     错误信息缓冲（可为 NULL）
 * @param[in]  errsz   err 大小
 * @return 0 成功；<0 失败（err 给出原因与寄存器诊断）
 */
int esi_flash_begin(ecx_contextt *context, int slave, char *err, size_t errsz);

/**
 * 退出烧录流程：把 EEPROM 访问权交还 PDI（从站需重新上电生效）
 *
 * @param[in] context SOEM 上下文
 * @param[in] slave   目标从站编号（1 起）
 */
void esi_flash_end(ecx_contextt *context, int slave);

/**
 * 构建 SII 镜像（身份区 0x0008-0x000D + 类别区 0x0040 起）
 *
 * PDI 配置 + CRC 校验区（字 0x0000-0x0007）与序列号不读不写；
 * 身份区按 XML 的厂商/产品/修订版本填写，类别区按 XML 生成。
 *
 * @param[in]  context   SOEM 上下文（用于身份校验）
 * @param[in]  slave     目标从站编号（1 起）
 * @param[in]  xml_path  ESI XML 文件路径（如 "xml/OLIF-WaterTank.xml"）
 * @param[out] img       输出镜像（按绝对字地址索引，仅 0x0040 起有效）
 * @param[in]  max_words img 容量（字数）
 * @param[in]  allow_mismatch 0 = XML 身份与从站不符时拒绝（返回
 *                       ESI_FLASH_ERR_MISMATCH）；1 = 不符也继续
 * @param[out] out_start 起始字地址（= 0x0040）
 * @param[out] out_words 实际字数（不含起始偏移）
 * @param[out] out_id_words 身份区字数（0 = XML 身份无效，不烧身份区）
 * @param[out] err       错误信息缓冲（可为 NULL）
 * @param[in]  errsz     err 大小
 * @return 0 成功；ESI_FLASH_ERR_MISMATCH 身份不符；其他 <0 失败
 */
int esi_flash_build_image(ecx_contextt *context, int slave, const char *xml_path,
                          uint16 *img, int max_words, int allow_mismatch,
                          int *out_start, int *out_words, int *out_id_words,
                          char *err, size_t errsz);

/**
 * 将镜像烧录到从站 EEPROM（逐字写入后逐字读回校验）
 *
 * 调用前须先执行 esi_flash_begin()。
 *
 * @param[in] context    SOEM 上下文
 * @param[in] slave      目标从站编号（1 起）
 * @param[in] img        镜像（按绝对字地址索引）
 * @param[in] start_word 起始字地址（0x0040）
 * @param[in] words      字数
 * @param[in] burn_identity 非 0 = 先烧身份区（img 0x0008-0x000D 须已填好）
 * @param[in] cb         进度回调（可为 NULL）
 * @param[in] user       回调用户参数
 * @param[out] err       错误信息缓冲（可为 NULL）
 * @param[in] errsz      err 大小
 * @return >=0 成功写入并校验的字数；<0 失败（err 给出原因）
 */
int esi_flash_burn(ecx_contextt *context, int slave, const uint16 *img,
                   int start_word, int words, int burn_identity,
                   esi_flash_progress_t cb, void *user,
                   char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* _esi_flash_ */
