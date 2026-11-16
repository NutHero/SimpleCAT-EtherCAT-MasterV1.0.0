/*
 * esi_flash.c - ESI XML → SII EEPROM 烧录实现
 *
 * SII EEPROM 布局（ETG.1000.6 / ESC 数据手册）：
 *   字 0x0000-0x0007  配置区（PDI 控制/配置、别名、CRC 校验和 @0x0007）
 *   字 0x0008-0x000F  身份区（Vendor/Product/Revision/Serial，ESC 上电自动装入寄存器）
 *   字 0x0010-0x001F  启动/标准邮箱地址与长度
 *   字 0x001C         标准邮箱协议支持位（CoE/FoE/EoE...）
 *   字 0x0020-0x003F  保留 / EEPROM 大小 / 版本
 *   字 0x0040 起      类别区：[类型 u16][字数 u16][数据...]，以 0xFFFF 结束
 *     0x000A STRINGS, 0x001E GENERAL, 0x0028 FMMU, 0x0029 SYNCM,
 *     0x0032 TxPDO, 0x0033 RxPDO
 *   PDO 类别数据：每 PDO 头 8 字节（索引 u16、条目数、SM、DC同步、名索引、
 *   标志、保留），每条目 8 字节（索引 u16、子索引、名索引、标志、位长、2 保留）。
 *
 * 烧录通道：ESC 寄存器 0x0500(EEPCFG 访问权) / 0x0502(EEPCTL 命令+状态) /
 * 0x0504(EEPADR) / 0x0508(EEPDAT)，由 SOEM 的 ecx_readeepromFP /
 * ecx_writeeepromFP 封装（自动忙等待与 NACK 重试）。
 *
 * 访问权（关键）：0x0500 bit0 = 0 归 EtherCAT 主站，1 归 PDI。
 * 从站进入 OP、PDI 固件跑起来后一般持有 EEPROM，主站读写全 NACK，
 * ecx_readeepromFP 只会返回 0（看起来像"读到全 0 / 产品代码 0x00000000"）。
 * 所以 esi_flash_begin() 必须先切 INIT + 强制收回访问权再读。
 */
#include "esi_flash.h"
#include "ui_lang.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <io.h> /* _findfirst / _findnext (Windows) */

#define ESI_SII_START     0x0040 /* 类别区起始字地址 = ECT_SII_START */

#define ESI_CAT_STRING    0x000A
#define ESI_CAT_GENERAL   0x001E
#define ESI_CAT_FMMU      0x0028
#define ESI_CAT_SM        0x0029
#define ESI_CAT_TXPDO     0x0032
#define ESI_CAT_RXPDO     0x0033
#define ESI_CAT_END       0xFFFF

/* EEPROM 访问超时（us），覆盖忙等待轮询 */
#define ESI_EEP_TIMEOUT   200000

/* ---------- 字节缓冲（类别数据构建用） ---------- */

typedef struct
{
   uint8 b[2048];
   int   len;
} esi_buf_t;

static void eb_u8(esi_buf_t *c, uint8 v)
{
   c->b[c->len++] = v;
}

static void eb_u16(esi_buf_t *c, uint16 v)
{
   c->b[c->len++] = (uint8)(v & 0xFF);
   c->b[c->len++] = (uint8)(v >> 8);
}

static void eb_u32(esi_buf_t *c, uint32 v)
{
   eb_u16(c, (uint16)(v & 0xFFFF));
   eb_u16(c, (uint16)(v >> 16));
}

static void eb_bytes(esi_buf_t *c, const char *s)
{
   while (*s)
      c->b[c->len++] = (uint8)*s++;
}

/* 把一个类别（数据字节缓冲）追加到字镜像，返回新 pos；失败 -1 */
static int esi_append_category(uint16 *img, int max_words, int pos,
                               uint16 type, const esi_buf_t *c)
{
   int words = (c->len + 1) / 2;
   int i;

   /* +1 为结尾 0xFFFF 终止符预留 */
   if (pos + 2 + words + 1 > max_words)
      return -1;

   img[pos++] = type;
   img[pos++] = (uint16)words;
   for (i = 0; i < words; i++)
   {
      uint8 lo = c->b[2 * i];
      uint8 hi = (2 * i + 1 < c->len) ? c->b[2 * i + 1] : 0;
      img[pos++] = (uint16)(lo | ((uint16)hi << 8));
   }
   return pos;
}

/* ---------- 类别数据构建 ---------- */

/* STRINGS：数量 u8，之后每串 长度 u8 + 字符。
   #1 = 组名（设备名去掉 "-" 后缀），#2 = 设备名（OrderIdx/NameIdx 引用） */
static void esi_build_strings(esi_buf_t *c, const tc3_device_t *dev)
{
   char group[TC3_NAME_LEN];
   char *dash;

   c->len = 0;
   snprintf(group, sizeof(group), "%s", dev->dev_name);
   dash = strchr(group, '-');
   if (dash != NULL)
      *dash = '\0';
   if (group[0] == '\0')
      snprintf(group, sizeof(group), "OLIF");

   eb_u8(c, 2);                                   /* 字符串总数 */
   eb_u8(c, (uint8)strlen(group));                /* #1 组名 */
   eb_bytes(c, group);
   eb_u8(c, (uint8)strlen(dev->dev_name));        /* #2 设备名 */
   eb_bytes(c, dev->dev_name);
}

/* GENERAL：20 字节。CoE Details = 0x03（SDO + SDO Info） */
static void esi_build_general(esi_buf_t *c)
{
   c->len = 0;
   eb_u8(c, 1);        /* [0]  GroupIdx  -> 字符串#1 */
   eb_u8(c, 0);        /* [1]  ImgIdx（无图片） */
   eb_u8(c, 2);        /* [2]  OrderIdx -> 字符串#2 */
   eb_u8(c, 2);        /* [3]  NameIdx  -> 字符串#2 */
   eb_u8(c, 0);        /* [4]  reserved */
   eb_u8(c, 0x03);     /* [5]  CoE Details: bit0 SDO, bit1 SDO Info */
   eb_u8(c, 0);        /* [6]  FoE Details */
   eb_u8(c, 0);        /* [7]  EoE Details */
   eb_u8(c, 0);        /* [8]  SoE Details */
   eb_u8(c, 0);        /* [9]  DS402 Channels */
   eb_u16(c, 0);       /* [10] CurrentOnEBus */
   eb_u16(c, 0);       /* [12] reserved */
   eb_u32(c, 0xFFFFFFFFu); /* [16] 物理端口区（未指定） */
}

/* FMMU：每 FMMU 一字节用途：1=输出(RxPDO) 2=输入(TxPDO) */
static void esi_build_fmmu(esi_buf_t *c, const tc3_device_t *dev)
{
   int i;
   int has_out = 0, has_in = 0;

   for (i = 0; i < dev->pdo_count; i++)
   {
      if (dev->pdo[i].entry_count == 0)
         continue;
      if (dev->pdo[i].dir == 0)
         has_out = 1;
      else
         has_in = 1;
   }

   c->len = 0;
   if (has_out)
      eb_u8(c, 1);
   if (has_in)
      eb_u8(c, 2);
}

/* SYNCM：每 SM 8 字节（起始地址 u16、长度 u16、控制、状态0、使能、类型）
   控制字节取 TwinCAT 标准值：SM0=0x26 SM1=0x22 SM2=0x64 SM3=0x20
   类型：1=MboxOut 2=MboxIn 3=Outputs 4=Inputs */
static void esi_build_sm(esi_buf_t *c, const tc3_device_t *dev)
{
   static const uint8 ctrl[4]  = { 0x26, 0x22, 0x64, 0x20 };
   static const uint8 smtype[4] = { 1, 2, 3, 4 };
   int i;

   c->len = 0;
   for (i = 0; i < dev->sm_count && i < 4; i++)
   {
      eb_u16(c, (uint16)dev->sm[i].start);
      eb_u16(c, (uint16)dev->sm[i].size);
      eb_u8(c, ctrl[i]);
      eb_u8(c, 0);            /* Status（只读，写 0） */
      eb_u8(c, 1);            /* Enable */
      eb_u8(c, smtype[i]);
   }
}

/* PDO 类别：dir 0=RxPDO(0x0033) 1=TxPDO(0x0032) */
static void esi_build_pdos(esi_buf_t *c, const tc3_device_t *dev, int dir)
{
   int i, j;

   c->len = 0;
   for (i = 0; i < dev->pdo_count; i++)
   {
      const tc3_pdo_t *p = &dev->pdo[i];

      if (p->dir != dir || p->entry_count == 0)
         continue;

      eb_u16(c, p->index);            /* PDO 索引 */
      eb_u8(c, (uint8)p->entry_count);
      eb_u8(c, (uint8)p->sm);         /* 所属 SM */
      eb_u8(c, 0);                    /* DC 同步引用：无 */
      eb_u8(c, 0);                    /* NameIdx：不命名 */
      eb_u8(c, 0x02);                 /* Flags: PdoDefault */
      eb_u8(c, 0);                    /* reserved */

      for (j = 0; j < p->entry_count; j++)
      {
         const tc3_entry_t *e = &p->entry[j];

         eb_u16(c, e->index);
         eb_u8(c, e->subindex);
         eb_u8(c, 0);                 /* NameIdx */
         eb_u8(c, 0);                 /* Flags */
         eb_u8(c, (uint8)e->bitlen);
         eb_u8(c, 0);                 /* reserved */
         eb_u8(c, 0);
      }
   }
}

/* ---------- EEPROM 访问权 ---------- */

/* 强制把 EEPROM 访问权从 PDI 收回给 EtherCAT 主站。
   SOEM 的 ecx_readeepromFP / ecx_writeeepromFP 不做这件事（只有按从站序号
   的 ecx_readeeprom() 内部才会调 ecx_eeprom2master），所以直接用 FPWR 写
   0x0500：先写 2（force from PDI），再写 0（assign to master）。 */
static int esi_take_eeprom(ecx_contextt *context, ec_slavet *sl)
{
   uint8 eepcfg;
   int wkc = 0;
   int cnt;

   eepcfg = 2;
   for (cnt = 0; cnt < 3; cnt++)
   {
      wkc = ecx_FPWR(&context->port, sl->configadr, ECT_REG_EEPCFG,
                     sizeof(eepcfg), &eepcfg, EC_TIMEOUTRET);
      if (wkc > 0)
         break;
   }
   osal_usleep(2000);

   eepcfg = 0;
   for (cnt = 0; cnt < 3; cnt++)
   {
      wkc = ecx_FPWR(&context->port, sl->configadr, ECT_REG_EEPCFG,
                     sizeof(eepcfg), &eepcfg, EC_TIMEOUTRET);
      if (wkc > 0)
         break;
   }
   osal_usleep(2000);

   /* 清掉可能残留的 EEPROM 命令/错误位（NOP，值 0 与字节序无关） */
   {
      uint16 nop = 0;
      ecx_FPWR(&context->port, sl->configadr, ECT_REG_EEPCTL,
               sizeof(nop), &nop, EC_TIMEOUTRET);
   }
   osal_usleep(2000);

   sl->eep_pdi = 0; /* 标记：现在是主站持有 */
   return (wkc > 0) ? 1 : 0;
}

/* 读 EEPROM 配置/状态寄存器用于诊断（失败时返回 0） */
static void esi_read_eep_regs(ecx_contextt *context, uint16 adr,
                              uint8 *cfg, uint16 *stat)
{
   uint8 b = 0;
   uint16 s = 0;

   if (ecx_FPRD(&context->port, adr, ECT_REG_EEPCFG, sizeof(b), &b,
                EC_TIMEOUTRET) > 0)
      *cfg = b;
   else
      *cfg = 0xFF;
   if (ecx_FPRD(&context->port, adr, ECT_REG_EEPCTL, sizeof(s), &s,
                EC_TIMEOUTRET) > 0)
      *stat = s;
   else
      *stat = 0xFFFF;
}

/* 探测 EEPROM 是否真的可读：身份区（0x0008 厂商 / 0x000A 产品代码）必须
   至少有一个非 0。读不到时重试若干次（刚收回访问权时 ESC 需要一点时间）。 */
static int esi_probe_eeprom(ecx_contextt *context, ec_slavet *sl,
                            char *err, size_t errsz)
{
   uint16 prod_lo = 0;
   uint16 man_lo = 0;
   int t;

   for (t = 0; t < 5; t++)
   {
      prod_lo = (uint16)(ecx_readeepromFP(context, sl->configadr,
                                          (uint16)ECT_SII_ID,
                                          ESI_EEP_TIMEOUT) & 0xFFFFu);
      man_lo = (uint16)(ecx_readeepromFP(context, sl->configadr,
                                         (uint16)ECT_SII_MANUF,
                                         ESI_EEP_TIMEOUT) & 0xFFFFu);
      if ((prod_lo | man_lo) != 0)
         return 0;
      osal_usleep(3000);
   }

   {
      uint8 cfg = 0;
      uint16 stat = 0;

      esi_read_eep_regs(context, sl->configadr, &cfg, &stat);
      if (err != NULL)
         snprintf(err, errsz,
                  TR("EEPROM 不可读（字 0x%04X 读回 0x0000；EEPCFG=0x%02X "
                     "EEPCTL=0x%04X 从站状态=0x%02X）",
                     "EEPROM not readable (word 0x%04X reads 0x0000; "
                     "EEPCFG=0x%02X EEPROM CTL=0x%04X slave state=0x%02X)"),
                  (unsigned)ECT_SII_ID, cfg, stat, sl->state);
   }
   return -1;
}

/* ---------- 对外接口 ---------- */

int esi_flash_begin(ecx_contextt *context, int slave, char *err, size_t errsz)
{
   ec_slavet *sl;

   if (context == NULL || err == NULL)
      return -1;
   err[0] = '\0';

   if (slave < 1 || slave > context->slavecount)
   {
      snprintf(err, errsz, TR("从站编号 %d 无效", "Invalid slave number %d"), slave);
      return -1;
   }
   sl = &context->slavelist[slave];

   /* 1) 切到 INIT：停止过程数据，PDI 不再周期性访问 EEPROM。
         重试 3 次：从站可能刚被上层恢复流程拉去 SAFE-OP/OP，
         状态机切换需要一点时间才能稳定接受新请求 */
   if ((sl->state & 0x0F) != EC_STATE_INIT)
   {
      int t;

      for (t = 0; t < 3; t++)
      {
         sl->state = EC_STATE_INIT;
         ecx_writestate(context, (uint16)slave);
         ecx_statecheck(context, (uint16)slave, EC_STATE_INIT, EC_TIMEOUTSTATE);
         if ((sl->state & 0x0F) == EC_STATE_INIT)
            break;
         osal_usleep(50000); /* 等 50ms 再试，躲开正在进行的其他状态切换 */
      }
      if ((sl->state & 0x0F) != EC_STATE_INIT)
      {
         snprintf(err, errsz, TR("从站 %d 无法切到 INIT（当前状态 0x%02X）",
                                 "Slave %d cannot switch to INIT (state 0x%02X)"),
                  slave, sl->state);
         return -1;
      }
   }

   /* 2) 取 EEPROM 访问权 */
   if (!esi_take_eeprom(context, sl))
   {
      snprintf(err, errsz, TR("无法访问从站 %d 的 EEPROM 寄存器（FPWR 0x0500 失败）",
                              "Cannot access EEPROM regs of slave %d (FPWR 0x0500 failed)"),
               slave);
      return -2;
   }

   /* 3) 探测可读性（这一步失败，后面写入必然失败，尽早报错） */
   if (esi_probe_eeprom(context, sl, err, errsz) != 0)
      return -3;

   return 0;
}

void esi_flash_end(ecx_contextt *context, int slave)
{
   if (context == NULL || slave < 1 || slave > context->slavecount)
      return;
   /* 交还 PDI（与 SOEM ecx_config_init 结束时的处理一致） */
   ecx_eeprom2pdi(context, (uint16)slave);
}

int esi_flash_list_xml(const char *dir,
                       char files[][ESI_FLASH_NAME_MAX], int max_files)
{
   char pattern[1024], path[1024];
   struct _finddata_t fd;
   intptr_t h;
   tc3_device_t dev;
   int n = 0;

   if (dir == NULL || dir[0] == '\0' || files == NULL || max_files <= 0)
      return 0;

   snprintf(pattern, sizeof(pattern), "%s/*.xml", dir);
   h = _findfirst(pattern, &fd);
   if (h == -1)
      return 0;

   do
   {
      if (n >= max_files)
         break;
      snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
      /* 只列出能解析出 <Device> 的 ESI 文件 */
      if (tc3_load_device_from_file(path, &dev) == 0 && dev.product_code != 0)
      {
         snprintf(files[n], ESI_FLASH_NAME_MAX, "%s", fd.name);
         n++;
      }
   } while (_findnext(h, &fd) == 0);

   _findclose(h);
   return n;
}

int esi_flash_build_image(ecx_contextt *context, int slave, const char *xml_path,
                          uint16 *img, int max_words, int allow_mismatch,
                          int *out_start, int *out_words, int *out_id_words,
                          char *err, size_t errsz)
{
   ec_slavet *sl;
   tc3_device_t dev;
   esi_buf_t cat;
   int pos;

#define SET_ERR(...) do { if (err != NULL) snprintf(err, errsz, __VA_ARGS__); } while (0)

   if (out_id_words != NULL)
      *out_id_words = 0;
   if (context == NULL || img == NULL || out_start == NULL || out_words == NULL ||
       xml_path == NULL || xml_path[0] == '\0')
   {
      SET_ERR(TR("参数错误", "Invalid argument"));
      return -1;
   }
   if (slave < 1 || slave > context->slavecount)
   {
      SET_ERR(TR("从站编号 %d 无效", "Invalid slave number %d"), slave);
      return -1;
   }
   if (max_words <= ESI_SII_START || max_words > ESI_FLASH_MAX_WORDS)
   {
      SET_ERR(TR("镜像容量参数错误（%d 字）", "Invalid image size (%d words)"), max_words);
      return -1;
   }
   sl = &context->slavelist[slave];

   /* 1) 身份基准取自 SOEM 在 config_init 时读到的 SII 身份。
         这里刻意不去读 EEPROM 固定区：从站处于 OP 或 EEPROM 归 PDI 时
         读回全是 0，会把正常的从站误判成"产品代码 0x00000000"。
         EEPROM 的真实可读性由 esi_flash_begin() 探测。 */
   if (sl->eep_id == 0)
   {
      SET_ERR(TR("从站在线产品代码为 0（EEPROM 身份未读到），已中止烧录",
                 "Slave product code reads 0 (EEPROM identity unread), flashing aborted"));
      return -2;
   }

   /* 清空镜像；身份区(0x0008-0x000D)和类别区(0x0040 起)会被填充，
         PDI 配置 + CRC 校验区(0x0000-0x0007)与序列号(0x000E-0x000F)不写 */
   memset(img, 0, (size_t)max_words * sizeof(uint16));

   /* 2) 解析 XML 并做身份安全校验 */
   if (tc3_load_device_from_file(xml_path, &dev) != 0)
   {
      SET_ERR(TR("无法从 %s 解析出设备描述 <Device>",
                 "Cannot parse <Device> description from %s"), xml_path);
      return -3;
   }
   if (dev.product_code == 0)
   {
      SET_ERR(TR("%s 中未解析出有效的产品代码",
                 "No valid product code parsed from %s"), xml_path);
      return -3;
   }
   if (dev.product_code != sl->eep_id || dev.vendor_id != sl->eep_man)
   {
      /* 不一致时默认拒绝，防止误烧别的设备的描述文件；
         换装新 ESI 时身份本来就不会一致，allow_mismatch=1 放行
         （TwinCAT 的 Write EEPROM 同样允许给从站写任意 ESI） */
      if (!allow_mismatch)
      {
         if (dev.product_code != sl->eep_id)
         {
            SET_ERR(TR("XML 产品代码 0x%08X 与从站 0x%08X 不符",
                       "XML product code 0x%08X does not match slave 0x%08X"),
                    dev.product_code, sl->eep_id);
         }
         else
         {
            SET_ERR(TR("XML 厂商 ID 0x%08X 与从站 0x%08X 不符",
                       "XML vendor ID 0x%08X does not match slave 0x%08X"),
                    dev.vendor_id, sl->eep_man);
         }
         return ESI_FLASH_ERR_MISMATCH;
      }
   }

   /* 3) 身份区（字 0x0008-0x000D）：写入 XML 的厂商/产品/修订版本，
         烧录后从站身份与新 ESI 一致，主站扫描即可匹配到新 XML。
         这些字不参与字 0x0007 的 CRC 校验，写错也不会导致 ESC 无法启动。
         序列号（0x000E-0x000F）保持原值不动。 */
   if (out_id_words != NULL && dev.vendor_id != 0)
   {
      img[ESI_FLASH_ID_START + 0] = (uint16)(dev.vendor_id & 0xFFFFu);
      img[ESI_FLASH_ID_START + 1] = (uint16)(dev.vendor_id >> 16);
      img[ESI_FLASH_ID_START + 2] = (uint16)(dev.product_code & 0xFFFFu);
      img[ESI_FLASH_ID_START + 3] = (uint16)(dev.product_code >> 16);
      img[ESI_FLASH_ID_START + 4] = (uint16)(dev.revision & 0xFFFFu);
      img[ESI_FLASH_ID_START + 5] = (uint16)(dev.revision >> 16);
      *out_id_words = ESI_FLASH_ID_WORDS;
   }

   /* 4) 生成类别区（从字 0x0040 开始） */
   pos = ESI_SII_START;

   esi_build_strings(&cat, &dev);
   pos = esi_append_category(img, max_words, pos, ESI_CAT_STRING, &cat);
   if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }

   esi_build_general(&cat);
   pos = esi_append_category(img, max_words, pos, ESI_CAT_GENERAL, &cat);
   if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }

   esi_build_fmmu(&cat, &dev);
   if (cat.len > 0)
   {
      pos = esi_append_category(img, max_words, pos, ESI_CAT_FMMU, &cat);
      if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }
   }

   esi_build_sm(&cat, &dev);
   if (cat.len > 0)
   {
      pos = esi_append_category(img, max_words, pos, ESI_CAT_SM, &cat);
      if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }
   }

   esi_build_pdos(&cat, &dev, 1); /* TxPDO */
   if (cat.len > 0)
   {
      pos = esi_append_category(img, max_words, pos, ESI_CAT_TXPDO, &cat);
      if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }
   }

   esi_build_pdos(&cat, &dev, 0); /* RxPDO */
   if (cat.len > 0)
   {
      pos = esi_append_category(img, max_words, pos, ESI_CAT_RXPDO, &cat);
      if (pos < 0) { SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words); return -5; }
   }

   if (pos + 1 > max_words)
   {
      SET_ERR(TR("镜像超出 EEPROM 容量（%d 字）", "Image exceeds EEPROM size (%d words)"), max_words);
      return -5;
   }
   img[pos++] = ESI_CAT_END; /* 类别区终止符 */

   *out_start = ESI_SII_START;
   *out_words = pos - ESI_SII_START;
   return 0;
#undef SET_ERR
}

int esi_flash_burn(ecx_contextt *context, int slave, const uint16 *img,
                   int start_word, int words, int burn_identity,
                   esi_flash_progress_t cb, void *user,
                   char *err, size_t errsz)
{
   ec_slavet *sl;
   int i, s;
   int seg_start[2], seg_words[2], nseg = 0;
   int total = 0;
   int done = 0;

#define SET_ERR(...) do { if (err != NULL) snprintf(err, errsz, __VA_ARGS__); } while (0)

   if (context == NULL || img == NULL || words <= 0 || start_word < 0 ||
       start_word + words > ESI_FLASH_MAX_WORDS ||
       slave < 1 || slave > context->slavecount)
   {
      SET_ERR(TR("参数错误", "Invalid argument"));
      return -1;
   }
   sl = &context->slavelist[slave];

   /* 烧录分段：先身份区（可选），再类别区 */
   if (burn_identity)
   {
      seg_start[nseg] = ESI_FLASH_ID_START;
      seg_words[nseg++] = ESI_FLASH_ID_WORDS;
   }
   seg_start[nseg] = start_word;
   seg_words[nseg++] = words;
   for (i = 0; i < nseg; i++)
      total += seg_words[i];

   /* 1) 确保从站处于 INIT 且 EEPROM 归主站（esi_flash_begin 已做过，
         这里做幂等保护：直接调 build_image/burn 时也能工作） */
   if ((sl->state & 0x0F) != EC_STATE_INIT)
   {
      sl->state = EC_STATE_INIT;
      ecx_writestate(context, (uint16)slave);
      ecx_statecheck(context, (uint16)slave, EC_STATE_INIT, EC_TIMEOUTSTATE);
      if ((sl->state & 0x0F) != EC_STATE_INIT)
      {
         SET_ERR(TR("无法将从站切换到 INIT（当前 0x%02X）",
                    "Cannot switch slave to INIT (state 0x%02X)"), sl->state);
         return -1;
      }
   }
   if (!esi_take_eeprom(context, sl))
   {
      SET_ERR(TR("无法取得 EEPROM 访问权（FPWR 0x0500 失败）",
                 "Cannot get EEPROM access (FPWR 0x0500 failed)"));
      return -2;
   }

   /* 2) 逐字写入（身份区 + 类别区；CRC/PDI 配置区与序列号不动） */
   for (s = 0; s < nseg; s++)
   {
      for (i = 0; i < seg_words[s]; i++)
      {
         int waddr = seg_start[s] + i;
         int try;

         for (try = 0; try < 3; try++)
         {
            if (ecx_writeeepromFP(context, sl->configadr, (uint16)waddr,
                                  img[waddr], ESI_EEP_TIMEOUT))
               break;
            osal_usleep(2000);
         }
         if (try >= 3)
         {
            SET_ERR(TR("第 0x%04X 字写入失败（连续 3 次 NACK）",
                       "Write failed at word 0x%04X (3 consecutive NACKs)"), waddr);
            return -3;
         }
         done++;
         if (cb != NULL && ((done & 0x03) == 0x03))
            cb(user, done, total);
      }
   }
   if (cb != NULL)
      cb(user, total, total);

   /* 3) 逐字读回校验 */
   done = 0;
   for (s = 0; s < nseg; s++)
   {
      for (i = 0; i < seg_words[s]; i++)
      {
         int waddr = seg_start[s] + i;
         uint16 w = 0;
         int try;

         for (try = 0; try < 3; try++)
         {
            w = (uint16)(ecx_readeepromFP(context, sl->configadr, (uint16)waddr,
                                          ESI_EEP_TIMEOUT) & 0xFFFFu);
            if (w == img[waddr])
               break;
            osal_usleep(2000);
         }
         if (w != img[waddr])
         {
            SET_ERR(TR("校验失败：字 0x%04X 读回 0x%04X，期望 0x%04X",
                       "Verify failed: word 0x%04X read 0x%04X, expected 0x%04X"),
                    waddr, w, img[waddr]);
            return -4;
         }
         done++;
         if (cb != NULL && ((done & 0x0F) == 0x0F))
            cb(user, total + done, total * 2); /* 校验阶段进度走后半程 */
      }
   }

   return total;
#undef SET_ERR
}
