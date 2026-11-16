/*
 * tc3_scan.c - TwinCAT3 风格的 EtherCAT 总线扫描显示模块
 *
 * 实现：
 *  - 轻量 ESI XML 解析器（无第三方依赖）：解析 xml/ 目录下
 *    EtherCATInfo 设备描述文件中的 <Device>、<Sm>、<RxPdo>、
 *    <TxPdo>、<Entry> 元素。
 *  - 按厂商 ID + 产品代码匹配在线从站与 ESI 设备描述。
 *  - 未匹配到 ESI 时，通过 CoE SDO 读取 0x1C12/0x1C13
 *    （SM2/SM3 PDO 分配对象）及各 PDO 映射对象，还原实际
 *    PDO 变量布局（此时无变量名，仅有索引/位宽）。
 *  - 将每个 PDO Entry 映射到主站 IOmap 过程映像的位偏移，
 *    类似 TwinCAT 的 Process Image 视图。
 */
#include "tc3_scan.h"
#include "ui_lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h> /* _findfirst / _findnext (Windows) */

/* ESI 解析用数据结构已移至 tc3_scan.h（供 esi_flash 烧录模块共用） */

/* ---------- 小工具函数 ---------- */

/* 解析 "#x1A00" / "0x1A00" / 十六进制或十进制文本 */
static uint32 tc3_parse_num(const char *text)
{
   if (text == NULL)
      return 0;
   while (*text == ' ' || *text == '\t')
      text++;
   if ((text[0] == '#') && (text[1] == 'x' || text[1] == 'X'))
      return (uint32)strtoul(text + 2, NULL, 16);
   if ((text[0] == '0') && (text[1] == 'x' || text[1] == 'X'))
      return (uint32)strtoul(text + 2, NULL, 16);
   if ((text[0] == '&') && (text[1] == 'x' || text[1] == 'X'))
      return (uint32)strtoul(text + 2, NULL, 16);
   return (uint32)strtoul(text, NULL, 0);
}

/* 去除首尾空白 */
static void tc3_trim(char *s)
{
   char *p;
   size_t len;

   if (s == NULL)
      return;
   p = s;
   while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
   len = strlen(s);
   while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                      s[len - 1] == '\r' || s[len - 1] == '\n'))
   {
      s[--len] = '\0';
   }
}

/* 拷贝并截断，保证以 0 结尾 */
static void tc3_copy_name(char *dst, size_t dstsize, const char *src)
{
   size_t n;

   if (dst == NULL || dstsize == 0)
      return;
   n = strlen(src);
   if (n >= dstsize)
      n = dstsize - 1;
   memcpy(dst, src, n);
   dst[n] = '\0';
}

/*
 * 在 [block, block_end) 区间内查找第一个 <tag ...>text</tag>，
 * 把 text（去除空白）拷贝到 out。
 * 返回 0 表示找到，-1 表示未找到。
 */
static int tc3_get_child_text(const char *block, const char *block_end,
                              const char *tag, char *out, size_t outsize)
{
   char open[64], close[64];
   const char *p, *t;
   size_t taglen;

   out[0] = '\0';
   taglen = strlen(tag);
   /* 形如 <Index 或 <Name —— 后面必须跟 '>'、' ' 或 '/'，防止误匹配前缀 */
   snprintf(open, sizeof(open), "<%s", tag);
   snprintf(close, sizeof(close), "</%s>", tag);

   p = block;
   while (p < block_end)
   {
      p = strstr(p, open);
      if (p == NULL || p >= block_end)
         return -1;
      /* 确认是独立标签而非前缀：标签名后的字符须为 '>'、' '、'/' 等。
       * 例如 "<Index>" 中 p[1+taglen] 为 '>'；"<IndexXxx>" 则为 'X'（拒绝） */
      {
         char c = p[1 + taglen];
         if (c != '>' && c != ' ' && c != '/' && c != '\t' && c != '\n')
         {
            p += 1 + taglen;
            continue;
         }
      }
      t = strchr(p, '>');
      if (t == NULL || t >= block_end)
         return -1;
      if (t[-1] == '/') /* 自闭合标签，无文本 */
         return -1;
      t++;
      p = strstr(t, close);
      if (p == NULL || p >= block_end)
         return -1;
      {
         size_t n = (size_t)(p - t);
         if (n >= outsize)
            n = outsize - 1;
         memcpy(out, t, n);
         out[n] = '\0';
      }
      tc3_trim(out);
      return 0;
   }
   return -1;
}

/* 读取标签属性值，如 attr="value" */
static int tc3_get_attr(const char *block, const char *block_end,
                        const char *attr, char *out, size_t outsize)
{
   char pat[64];
   const char *p, *q;
   size_t n;

   out[0] = '\0';
   snprintf(pat, sizeof(pat), "%s=\"", attr);
   p = strstr(block, pat);
   if (p == NULL || p >= block_end)
      return -1;
   p += strlen(pat);
   q = strchr(p, '"');
   if (q == NULL || q >= block_end)
      return -1;
   n = (size_t)(q - p);
   if (n >= outsize)
      n = outsize - 1;
   memcpy(out, p, n);
   out[n] = '\0';
   return 0;
}

/* 读取标签属性值并按数字解析 */
static uint32 tc3_get_attr_num(const char *block, const char *block_end,
                               const char *attr)
{
   char buf[64];

   if (tc3_get_attr(block, block_end, attr, buf, sizeof(buf)) != 0)
      return 0;
   return tc3_parse_num(buf);
}

/* ---------- ESI 设备块解析 ---------- */

/* 解析一个 <Sm ...>name</Sm> 块 */
static void tc3_parse_sm(const char *block, const char *block_end, tc3_sm_t *sm)
{
   sm->start = (int)tc3_get_attr_num(block, block_end, "StartAddress");
   sm->size = (int)tc3_get_attr_num(block, block_end, "DefaultSize");
   if (sm->size == 0)
      sm->size = (int)tc3_get_attr_num(block, block_end, "MaxSize");
   if (tc3_get_child_text(block, block_end, "Sm", sm->name, sizeof(sm->name)) != 0)
      sm->name[0] = '\0';
}

/* 解析一个 <Entry> ... </Entry> 块 */
static void tc3_parse_entry(const char *block, const char *block_end,
                            tc3_entry_t *e)
{
   char buf[64];

   memset(e, 0, sizeof(*e));
   if (tc3_get_child_text(block, block_end, "Index", buf, sizeof(buf)) == 0)
      e->index = (uint16)tc3_parse_num(buf);
   if (tc3_get_child_text(block, block_end, "SubIndex", buf, sizeof(buf)) == 0)
      e->subindex = (uint8)tc3_parse_num(buf);
   if (tc3_get_child_text(block, block_end, "BitLen", buf, sizeof(buf)) == 0)
      e->bitlen = (int)tc3_parse_num(buf);
   if (tc3_get_child_text(block, block_end, "Name", e->name, sizeof(e->name)) != 0)
      e->name[0] = '\0';
   if (tc3_get_child_text(block, block_end, "DataType", e->type, sizeof(e->type)) != 0)
      e->type[0] = '\0';
}

/* 解析一个 <RxPdo>/<TxPdo> ... </RxPdo> 块 */
static void tc3_parse_pdo(const char *block, const char *block_end, int dir,
                          tc3_pdo_t *pdo)
{
   const char *p, *e;
   char buf[64];

   memset(pdo, 0, sizeof(*pdo));
   pdo->dir = dir;
   pdo->sm = (int)tc3_get_attr_num(block, block_end, "Sm");
   if (tc3_get_child_text(block, block_end, "Index", buf, sizeof(buf)) == 0)
      pdo->index = (uint16)tc3_parse_num(buf);
   if (tc3_get_child_text(block, block_end, "Name", pdo->name, sizeof(pdo->name)) != 0)
      pdo->name[0] = '\0';

   pdo->entry_count = 0;
   p = block;
   while (pdo->entry_count < TC3_MAX_ENTRY)
   {
      p = strstr(p, "<Entry>");
      if (p == NULL || p >= block_end)
         break;
      e = strstr(p, "</Entry>");
      if (e == NULL || e >= block_end)
         break;
      tc3_parse_entry(p, e, &pdo->entry[pdo->entry_count]);
      pdo->entry_count++;
      p = e + 1;
   }
}

/* 解析一个 <Device ...> ... </Device> 块 */
static void tc3_parse_device(const char *block, const char *block_end,
                             tc3_device_t *dev)
{
   const char *p, *e;

   memset(dev, 0, sizeof(*dev));

   /* 设备类型名： <Type ProductCode="...">Name</Type> */
   p = strstr(block, "<Type ProductCode=");
   if (p != NULL && p < block_end)
   {
      e = strstr(p, "</Type>");
      if (e != NULL && e < block_end)
      {
         /* block_end 必须越过 "</Type>"，闭合标签搜索才能成功 */
         tc3_get_child_text(p, e + 7, "Type", dev->dev_name, sizeof(dev->dev_name));
      }
      dev->product_code = tc3_get_attr_num(p, block_end, "ProductCode");
      dev->revision = tc3_get_attr_num(p, block_end, "RevisionNo");
   }

   /* SyncManager */
   dev->sm_count = 0;
   p = block;
   while (dev->sm_count < TC3_MAX_SM)
   {
      p = strstr(p, "<Sm ");
      if (p == NULL || p >= block_end)
         break;
      e = strchr(p, '>'); /* 简单处理：Sm 标签内不会嵌套 '>' */
      if (e == NULL || e >= block_end)
         break;
      e = strstr(e, "</Sm>");
      if (e == NULL || e >= block_end)
         break;
      e += 5; /* 越过 "</Sm>" */
      tc3_parse_sm(p, e, &dev->sm[dev->sm_count]);
      dev->sm_count++;
      p = e;
   }

   /* PDO：RxPdo = 输出，TxPdo = 输入 */
   dev->pdo_count = 0;
   p = block;
   while (dev->pdo_count < TC3_MAX_PDO)
   {
      const char *rx = strstr(p, "<RxPdo");
      const char *tx = strstr(p, "<TxPdo");

      if (rx != NULL && (tx == NULL || rx < tx) && rx < block_end)
      {
         e = strstr(rx, "</RxPdo>");
         if (e == NULL || e >= block_end)
            break;
         e += 8;
         tc3_parse_pdo(rx, e, 0, &dev->pdo[dev->pdo_count]);
      }
      else if (tx != NULL && tx < block_end)
      {
         e = strstr(tx, "</TxPdo>");
         if (e == NULL || e >= block_end)
            break;
         e += 8;
         tc3_parse_pdo(tx, e, 1, &dev->pdo[dev->pdo_count]);
      }
      else
      {
         break;
      }
      dev->pdo_count++;
      p = e;
   }
}

/* ---------- ESI 文件加载与设备匹配 ---------- */

/* 在一段 XML 文本中查找匹配 (vendor, product) 的设备块 */
static int tc3_find_device_in_buf(const char *buf, uint32 vendor, uint32 product,
                                  tc3_device_t *dev)
{
   char vbuf[32];
   const char *p = buf;
   const char *vend_end;
   uint32 file_vendor = 0;

   /* 全局厂商 ID：<Vendor><Id>#x1</Id> */
   vend_end = strstr(buf, "<Descriptions>");
   if (vend_end == NULL)
      vend_end = buf + strlen(buf);
   if (tc3_get_child_text(buf, vend_end, "Id", vbuf, sizeof(vbuf)) == 0)
      file_vendor = tc3_parse_num(vbuf);

   while (1)
   {
      const char *e;
      p = strstr(p, "<Device");
      if (p == NULL)
         break;
      /* 确认是 <Device> 或 <Device ...>，而非 <DeviceXxx> 前缀 */
      {
         char c = p[7];
         if (c != '>' && c != ' ')
         {
            p += 7;
            continue;
         }
      }
      e = strstr(p, "</Device>");
      if (e == NULL)
         break;
      tc3_parse_device(p, e, dev);
      if (dev->product_code == product)
      {
         /* 厂商 ID 匹配：有效候选 = Device 内 VendorId 与文件级 <Vendor><Id>
            中非 0 者。任何一个候选与从站厂商不同即判不匹配（此前
            dev->vendor_id == 0 时无条件放行，会把别的厂商的同产品代码
            ESI 误判为匹配，导致"XML 与从站不匹配"警告无法出现） */
         uint32 cand[2];
         int ncand = 0, vend_ok = 0, ci;

         if (dev->vendor_id != 0)
            cand[ncand++] = dev->vendor_id;
         if (file_vendor != 0)
            cand[ncand++] = file_vendor;

         if (ncand == 0 || vendor == 0)
            vend_ok = 1; /* 文件完全没给厂商信息或从站身份未知：只能按产品代码匹配 */
         else
         {
            for (ci = 0; ci < ncand; ci++)
            {
               if (cand[ci] == vendor)
               {
                  vend_ok = 1;
                  break;
               }
            }
         }

         if (vend_ok)
         {
            if (dev->vendor_id == 0)
               dev->vendor_id = file_vendor;
            return 0;
         }
      }
      p = e + 1;
   }
   return -1;
}

/* 读取整个文件到内存（调用者 free） */
static char *tc3_load_file(const char *path)
{
   FILE *f;
   long size;
   char *buf;

   f = fopen(path, "rb");
   if (f == NULL)
      return NULL;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size <= 0 || size > 32 * 1024 * 1024)
   {
      fclose(f);
      return NULL;
   }
   buf = (char *)malloc((size_t)size + 1);
   if (buf == NULL)
   {
      fclose(f);
      return NULL;
   }
   if (fread(buf, 1, (size_t)size, f) != (size_t)size)
   {
      free(buf);
      fclose(f);
      return NULL;
   }
   buf[size] = '\0';
   fclose(f);
   return buf;
}

/* 在 xml_dir 下按文件名搜索匹配的 ESI 设备描述 */
int tc3_find_device_in_dir(const char *xml_dir, uint32 vendor,
                                  uint32 product, tc3_device_t *dev,
                                  char *matched_file, size_t matchsize)
{
   char pattern[1024], path[1024];
   struct _finddata_t fd;
   intptr_t h;
   char *buf;

   if (xml_dir == NULL || xml_dir[0] == '\0')
      return -1;

   snprintf(pattern, sizeof(pattern), "%s/*.xml", xml_dir);
   h = _findfirst(pattern, &fd);
   if (h == -1)
      return -1;

   do
   {
      snprintf(path, sizeof(path), "%s/%s", xml_dir, fd.name);
      buf = tc3_load_file(path);
      if (buf == NULL)
         continue;
      if (tc3_find_device_in_buf(buf, vendor, product, dev) == 0)
      {
         free(buf);
         _findclose(h);
         tc3_copy_name(matched_file, matchsize, fd.name);
         return 0;
      }
      free(buf);
   } while (_findnext(h, &fd) == 0);

   _findclose(h);
   return -1;
}

/* 从单个 ESI XML 文件解析设备描述（取第一个 <Device>） */
int tc3_load_device_from_file(const char *path, tc3_device_t *dev)
{
   char *buf;
   const char *p, *e, *vend_end;
   char vbuf[32];

   if (path == NULL || path[0] == '\0' || dev == NULL)
      return -1;

   buf = tc3_load_file(path);
   if (buf == NULL)
      return -2;

   memset(dev, 0, sizeof(*dev));
   p = strstr(buf, "<Device");
   while (p != NULL && p[7] != '>' && p[7] != ' ')
      p = strstr(p + 1, "<Device");
   if (p == NULL)
   {
      free(buf);
      return -3;
   }
   e = strstr(p, "</Device>");
   if (e == NULL)
   {
      free(buf);
      return -3;
   }

   tc3_parse_device(p, e, dev);

   /* 全局厂商 ID：<Vendor><Id>#x1</Id>（Device 内未单独给出时使用） */
   vend_end = strstr(buf, "<Descriptions>");
   if (vend_end == NULL)
      vend_end = buf + strlen(buf);
   if (tc3_get_child_text(buf, vend_end, "Id", vbuf, sizeof(vbuf)) == 0)
      dev->vendor_id = tc3_parse_num(vbuf);

   free(buf);
   return 0;
}

/* ---------- CoE 兜底：通过 SDO 读取实际 PDO 映射 ---------- */

/*
 * 读取 SM 分配对象 (0x1C12 = 输出, 0x1C13 = 输入) 得到 PDO 列表，
 * 再逐个读取 PDO 映射对象的子索引（每个子索引为
 * uint32：高16位=对象索引, 中8位=子索引, 低8位=位宽）。
 */
static int tc3_coe_read_pdos(ecx_contextt *context, uint16 slave, int dir,
                             tc3_pdo_t *pdos, int max_pdos)
{
   uint16 assign_idx = (dir == 0) ? 0x1C12 : 0x1C13;
   uint8 buf[64];
   int size, npdo, i, j, count = 0;
   uint16 pdo_idx;

   /* 0x1C12:0 = 已分配 PDO 数量 (USINT) */
   size = 1;
   if (ecx_SDOread(context, slave, assign_idx, 0x00, FALSE, &size, buf,
                   EC_TIMEOUTRXM) <= 0)
      return 0;
   npdo = (size >= 1) ? buf[0] : 0;
   if (npdo > max_pdos)
      npdo = max_pdos;

   for (i = 0; i < npdo; i++)
   {
      /* 0x1C12:1..n = PDO 索引 (UINT) */
      size = 2;
      if (ecx_SDOread(context, slave, assign_idx, (uint8)(i + 1), FALSE, &size,
                      buf, EC_TIMEOUTRXM) <= 0 || size < 2)
         continue;
      pdo_idx = (uint16)(buf[0] | (buf[1] << 8));

      memset(&pdos[count], 0, sizeof(pdos[count]));
      pdos[count].dir = dir;
      pdos[count].sm = (dir == 0) ? 2 : 3;
      pdos[count].index = pdo_idx;
      snprintf(pdos[count].name, TC3_NAME_LEN, "PDO 0x%04X (CoE)", pdo_idx);

      /* PDO 索引:0 = 映射条目数 */
      size = 1;
      if (ecx_SDOread(context, slave, pdo_idx, 0x00, FALSE, &size, buf,
                      EC_TIMEOUTRXM) > 0)
      {
         int nentry = (size >= 1) ? buf[0] : 0;
         if (nentry > TC3_MAX_ENTRY)
            nentry = TC3_MAX_ENTRY;
         for (j = 0; j < nentry; j++)
         {
            uint32 v;
            /* PDO 索引:1..n = 映射条目 (UDINT) */
            size = 4;
            if (ecx_SDOread(context, slave, pdo_idx, (uint8)(j + 1), FALSE,
                            &size, buf, EC_TIMEOUTRXM) <= 0 || size < 4)
               continue;
            v = (uint32)buf[0] | ((uint32)buf[1] << 8) |
                ((uint32)buf[2] << 16) | ((uint32)buf[3] << 24);
            pdos[count].entry[j].index = (uint16)(v >> 16);
            pdos[count].entry[j].subindex = (uint8)((v >> 8) & 0xFF);
            pdos[count].entry[j].bitlen = (int)(v & 0xFF);
            snprintf(pdos[count].entry[j].name, TC3_NAME_LEN,
                     "0x%04X:%02X", pdos[count].entry[j].index,
                     pdos[count].entry[j].subindex);
            snprintf(pdos[count].entry[j].type, sizeof(pdos[count].entry[j].type), "CoE");
            pdos[count].entry_count++;
         }
      }
      count++;
   }
   return count;
}

/* ---------- 打印辅助 ---------- */

static const char *tc3_state_string(uint16 state)
{
   switch (state & 0x0F)
   {
      case EC_STATE_NONE:    return "NONE";
      case EC_STATE_INIT:    return "INIT";
      case EC_STATE_PRE_OP:  return "PRE-OP";
      case EC_STATE_BOOT:    return "BOOT";
      case EC_STATE_SAFE_OP: return "SAFE-OP";
      case EC_STATE_OPERATIONAL: return "OP";
      default:               return "?";
   }
}

static const char *tc3_smtype_string(uint8 t)
{
   switch (t)
   {
      case 1: return "MbxOut";
      case 2: return "MbxIn";
      case 3: return "Process Out";
      case 4: return "Process In";
      default: return "Unused";
   }
}

static void tc3_mailbox_proto_string(uint16 proto, char *out, size_t outsize)
{
   out[0] = '\0';
   if (proto == 0)
   {
      snprintf(out, outsize, "none");
      return;
   }
   if (proto & ECT_MBXPROT_AOE) strncat(out, "AoE ", outsize - strlen(out) - 1);
   if (proto & ECT_MBXPROT_EOE) strncat(out, "EoE ", outsize - strlen(out) - 1);
   if (proto & ECT_MBXPROT_COE) strncat(out, "CoE ", outsize - strlen(out) - 1);
   if (proto & ECT_MBXPROT_FOE) strncat(out, "FoE ", outsize - strlen(out) - 1);
   if (proto & ECT_MBXPROT_SOE) strncat(out, "SoE ", outsize - strlen(out) - 1);
   if (proto & ECT_MBXPROT_VOE) strncat(out, "VoE ", outsize - strlen(out) - 1);
}

/* 打印一个 PDO 及其变量（含 IOmap 位偏移），返回消耗的总位数 */
static int tc3_print_pdo(const tc3_pdo_t *pdo, int base_bit)
{
   int bit = base_bit;
   int i;

   printf("  %s 0x%04X  \"%s\"  (SM%d)\n",
          pdo->dir == 0 ? "RxPDO" : "TxPDO", pdo->index, pdo->name, pdo->sm);
   if (pdo->entry_count == 0)
   {
      printf("      <empty>\n");
      return 0;
   }
   printf("      %-12s %-12s %-30s %-10s %s\n",
          "Offset", "Index:Sub", "Name", "Type", "BitLen");
   for (i = 0; i < pdo->entry_count; i++)
   {
      const tc3_entry_t *e = &pdo->entry[i];
      printf("      0x%04X.%-4d 0x%04X:%02X    %-30s %-10s %d\n",
             bit / 8, bit % 8, e->index, e->subindex,
             e->name[0] ? e->name : "(no name)",
             e->type[0] ? e->type : "-",
             e->bitlen);
      bit += e->bitlen;
   }
   return bit - base_bit;
}

/* ---------- 设备信息收集（ESI 优先，CoE 兜底） ---------- */

/*
 * 收集一个从站的 PDO 结构：优先匹配 xml_dir 下的 ESI 文件，
 * 未匹配则通过 CoE SDO 读取实际 PDO 映射。
 * 返回 1 = ESI 匹配成功；0 = CoE 兜底。
 */
static int tc3_collect_device(ecx_contextt *context, int slave_i, const char *xml_dir,
                              tc3_device_t *dev, char *esi_file, size_t esisz)
{
   ec_slavet *sl = &context->slavelist[slave_i];

   memset(dev, 0, sizeof(*dev));
   if (esi_file != NULL && esisz > 0)
      esi_file[0] = '\0';

   /* 变量来源 1：ESI XML */
   if (tc3_find_device_in_dir(xml_dir, sl->eep_man, sl->eep_id, dev,
                              esi_file, esisz) == 0)
      return 1;

   /* 变量来源 2：CoE SDO 兜底（读 0x1C12/0x1C13 及 PDO 映射对象） */
   if (sl->mbx_l > 0 && (sl->mbx_proto & ECT_MBXPROT_COE))
   {
      int n = tc3_coe_read_pdos(context, (uint16)slave_i, 0,
                                dev->pdo, TC3_MAX_PDO / 2);
      n += tc3_coe_read_pdos(context, (uint16)slave_i, 1,
                             dev->pdo + n, TC3_MAX_PDO - n);
      dev->pdo_count = n;
   }
   return 0;
}

/* 计算从站在组 IOmap 中的输出/输入基址（单位：位）。
   注意：本版本 SOEM 从不维护 slavelist[].Ooffset/Ioffset（恒为 0），
   直接用它算基址会让所有从站的变量都指向组 IOmap 偏移 0——
   多从站时全部读到第一个从站的数据（表现为"两个从站波形/数值完全一样"）。
   SOEM 实际为每个从站填好了 outputs/inputs 指针（相对组基址的差值
   已包含普通映射与 overlapping 映射两种布局），用指针差换算才是正确基址。 */
static void tc3_slave_io_base_bits(const ecx_contextt *context, const ec_slavet *sl,
                                   int *out_base, int *in_base)
{
   const ec_groupt *grp = &context->grouplist[sl->group];
   int rel_out = 0, rel_in = 0;

   if (sl->outputs != NULL && grp->outputs != NULL)
      rel_out = (int)(sl->outputs - grp->outputs);
   if (sl->inputs != NULL && grp->inputs != NULL)
      rel_in = (int)(sl->inputs - grp->inputs);

   *out_base = rel_out * 8 + (int)sl->Ostartbit;
   *in_base = rel_in * 8 + (int)sl->Istartbit;
}

/* 把一个从站的所有 PDO 变量加入变量表（含 IOmap 字节偏移） */
static void tc3_fill_varlist_slave(tc3_varlist_t *list,
                                   int slave_i, const tc3_device_t *dev,
                                   int bit_out_base, int bit_in_base)
{
   int t, j;
   int bit_out = bit_out_base;
   int bit_in = bit_in_base;

   for (t = 0; t < dev->pdo_count; t++)
   {
      const tc3_pdo_t *p = &dev->pdo[t];
      int bit = (p->dir == 0) ? bit_out : bit_in;

      for (j = 0; j < p->entry_count && list->count < TC3_MAX_VARS; j++)
      {
         const tc3_entry_t *e = &p->entry[j];
         tc3_variable_t *v = &list->var[list->count];

         memset(v, 0, sizeof(*v));
         tc3_copy_name(v->name, sizeof(v->name), e->name);
         if (v->name[0] == '\0')
            snprintf(v->name, sizeof(v->name), "0x%04X:%02X", e->index, e->subindex);
         v->index = e->index;
         v->sub = e->subindex;
         v->bitlen = e->bitlen;
         v->dir = p->dir;
         v->slave = (uint8)slave_i;
         v->offset = (uint32)(bit / 8);
         v->bitoff = (uint8)(bit % 8);
         tc3_copy_name(v->type, sizeof(v->type), e->type);
         list->count++;
         bit += e->bitlen;
      }
      if (p->dir == 0)
         bit_out = bit;
      else
         bit_in = bit;
   }
}

/* 在 ESI 设备里按 index:sub 查变量条目（用于给实际映射的变量配名字），查不到返回 NULL */
static const tc3_entry_t *tc3_esi_find_entry(const tc3_device_t *dev,
                                             uint16 index, uint8 subindex)
{
   int t, j;

   for (t = 0; t < dev->pdo_count; t++)
      for (j = 0; j < dev->pdo[t].entry_count; j++)
         if (dev->pdo[t].entry[j].index == index &&
             dev->pdo[t].entry[j].subindex == subindex)
            return &dev->pdo[t].entry[j];
   return NULL;
}

/* 统计一个 ESI 设备的输出/输入总位数（跳过 0x0000 填充项） */
static void tc3_esi_total_bits(const tc3_device_t *dev,
                               long *out_bits, long *in_bits)
{
   int t, j;

   *out_bits = 0;
   *in_bits = 0;
   for (t = 0; t < dev->pdo_count; t++)
   {
      long bits = 0;
      for (j = 0; j < dev->pdo[t].entry_count; j++)
         if (dev->pdo[t].entry[j].index != 0x0000)
            bits += dev->pdo[t].entry[j].bitlen;
      if (dev->pdo[t].dir == 0)
         *out_bits += bits;
      else
         *in_bits += bits;
   }
}

int tc3_scan_bus(ecx_contextt *context, const char *xml_dir)
{
   int i;
   int nslaves = context->slavecount;

   printf("\n");
   printf("==============================================================================\n");
   printf(" EtherCAT Bus Scan (TwinCAT3 style) - %d slave(s)\n", nslaves);
   printf("==============================================================================\n");

   for (i = 1; i <= nslaves; i++)
   {
      ec_slavet *sl = &context->slavelist[i];
      tc3_device_t dev;
      int have_esi;
      char matched_file[256] = "";
      char mbxproto[48];
      int t;
      int bit_out, bit_in;
      int printed_out = 0, printed_in = 0;

      printf("\n");
      printf("Slave #%d \"%s\"\n", i, sl->name);
      printf("------------------------------------------------------------------------------\n");
      printf("  Identity     : Vendor 0x%08X  Product 0x%08X  Rev 0x%08X  Serial 0x%08X\n",
             sl->eep_man, sl->eep_id, sl->eep_rev, sl->eep_ser);
      printf("  Address      : configured 0x%04X  alias 0x%04X   State: %s%s\n",
             sl->configadr, sl->aliasadr, tc3_state_string(sl->state),
             (sl->state & EC_STATE_ERROR) ? " (+ERROR)" : "");
      tc3_mailbox_proto_string(sl->mbx_proto, mbxproto, sizeof(mbxproto));
      printf("  Mailbox      : out 0x%04X (%u B)  in 0x%04X (%u B)  proto: %s\n",
             sl->mbx_wo, sl->mbx_l, sl->mbx_ro, sl->mbx_rl, mbxproto);
      printf("  DC           : %s   Port type: 0x%02X\n",
             sl->hasdc ? "supported" : "no", sl->ptype);
      tc3_slave_io_base_bits(context, sl, &bit_out, &bit_in);
      printf("  Process data : Outputs %u B (IOmap offset 0x%04X, startbit %d)"
             "  Inputs %u B (IOmap offset 0x%04X, startbit %d)\n",
             sl->Obytes, bit_out / 8, bit_out % 8,
             sl->Ibytes, bit_in / 8, bit_in % 8);

      /* SyncManager 表 */
      printf("  SyncManagers :\n");
      for (t = 0; t < EC_MAXSM; t++)
      {
         if (sl->SMtype[t] == 0)
            continue;
         printf("    SM%d  start 0x%04X  len %4u B   type: %s\n",
                t, sl->SM[t].StartAddr, sl->SM[t].SMlength,
                tc3_smtype_string(sl->SMtype[t]));
      }

      /* FMMU 表 */
      {
         int nfmmu = 0;
         printf("  FMMU         :\n");
         for (t = 0; t < EC_MAXFMMU; t++)
         {
            uint8 ftype = sl->FMMU[t].FMMUtype;
            if (ftype == 0)
               continue;
            nfmmu++;
            printf("    FMMU%d  logical 0x%08X  len %u B (startbit %d, endbit %d)  type: %s\n",
                   t, sl->FMMU[t].LogStart, sl->FMMU[t].LogLength,
                   sl->FMMU[t].LogStartbit, sl->FMMU[t].LogEndbit,
                   ftype == 1 ? "Outputs" : (ftype == 2 ? "Inputs" :
                                            (ftype == 3 ? "SM status" : "?")));
         }
         if (nfmmu == 0)
            printf("    <none>\n");
      }

      /* 收集 PDO 结构（ESI 优先，CoE 兜底） */
      have_esi = tc3_collect_device(context, i, xml_dir, &dev,
                                    matched_file, sizeof(matched_file));
      if (have_esi)
      {
         long esi_out_bits, esi_in_bits;

         printf("  ESI file     : %s (device \"%s\")\n", matched_file,
                dev.dev_name[0] ? dev.dev_name : "?");
         printf("  SM (ESI)     :");
         for (t = 0; t < dev.sm_count; t++)
            printf("  SM%d@0x%04X:%s", t, dev.sm[t].start,
                   dev.sm[t].name[0] ? dev.sm[t].name : "?");
         printf("\n");

         /* ESI PDO 定义与从站实际映射（SM2/SM3 长度）比对：
            不一致说明 XML 与从站固件版本不对应，会引起
            "Invalid input/output configuration" 或数据错位 */
         tc3_esi_total_bits(&dev, &esi_out_bits, &esi_in_bits);
         if (esi_out_bits != (long)sl->Obytes * 8 ||
             esi_in_bits != (long)sl->Ibytes * 8)
         {
            printf("  \x1b[33m[警告] ESI PDO 定义与从站固件实际映射不符！\x1b[0m\n");
            printf("               输出: ESI %ldbit vs 实际 %uB(%dbit)   "
                   "输入: ESI %ldbit vs 实际 %uB(%dbit)\n",
                   esi_out_bits, sl->Obytes, (int)(sl->Obytes * 8),
                   esi_in_bits, sl->Ibytes, (int)(sl->Ibytes * 8));
            printf("               实时变量表已按从站实际映射显示，请更新 XML。\n");
         }
      }
      else
      {
         /* XML 与从站身份不匹配：给出醒目警告（而不是静默回退），
            常见原因是 xml/ 目录里放了别的设备的 ESI，或从站 EEPROM
            里烧录的身份与手头 XML 不一致（如刚换烧过新 XML 未重新上电） */
         printf("  \x1b[33m[警告] ESI file  : 未在 \"%s\" 中找到与该从站匹配的设备描述！\x1b[0m\n",
                xml_dir ? xml_dir : "-");
         printf("               从站身份：厂商ID 0x%08X + 产品代码 0x%08X，xml/ 中没有\n",
                sl->eep_man, sl->eep_id);
         printf("               同时满足这两者的 <Device>。变量名回退为 CoE SDO 读取，\n");
         printf("               可能与实际 PDO 映射不一致，请核对 xml 文件是否放对。\n");
         printf("               （若刚用 flash 烧录过新 XML，请给从站重新上电后再重启主站）\n");
      }

      /* 打印所有 PDO 及变量（输出/输入统一遍历） */
      printf("  Output PDOs  : %u bytes, IOmap base 0x%04X.%d\n",
             sl->Obytes, bit_out / 8, bit_out % 8);
      printf("  Input PDOs   : %u bytes, IOmap base 0x%04X.%d\n",
             sl->Ibytes, bit_in / 8, bit_in % 8);
      for (t = 0; t < dev.pdo_count; t++)
      {
         if (dev.pdo[t].dir == 0)
         {
            bit_out += tc3_print_pdo(&dev.pdo[t], bit_out);
            printed_out = 1;
         }
         else
         {
            bit_in += tc3_print_pdo(&dev.pdo[t], bit_in);
            printed_in = 1;
         }
      }
      if (!printed_out)
         printf("      <no output PDO>\n");
      if (!printed_in)
         printf("      <no input PDO>\n");
   }

   printf("\n");
   printf("==============================================================================\n");
   printf(" End of bus scan\n");
   printf("==============================================================================\n\n");

   return nslaves;
}

/* ---------- 变量表构建（供实时读写界面使用） ---------- */

int tc3_build_varlist(ecx_contextt *context, const char *xml_dir, tc3_varlist_t *list)
{
   int i, nslaves;

   if (list == NULL)
      return -1;
   memset(list, 0, sizeof(*list));

   nslaves = context->slavecount;
   for (i = 1; i <= nslaves; i++)
   {
      ec_slavet *sl = &context->slavelist[i];
      tc3_device_t esi;
      char matched_file[256] = "";
      int have_esi;
      tc3_pdo_t actual[TC3_MAX_PDO];
      int nactual = 0, t, j;
      int bit_out, bit_in;

      have_esi = (tc3_find_device_in_dir(xml_dir, sl->eep_man, sl->eep_id,
                                         &esi, matched_file,
                                         sizeof(matched_file)) == 0);

      /* 优先用从站实际 CoE 映射（0x1C12/0x1C13 + PDO 对象）计算 IOmap 偏移，
         ESI 只用于给变量配名字。
         原因：ESI 的 PDO 定义可能与从站固件不一致（实测出现过 ESI 输入
         2 字节 vs 从站实际 38 字节）。若按 ESI 计算偏移，主站会写进
         错误的 IOmap 字节，表现为"下发后从站接收不到/动作错乱"。
         SOEM 的 config_map_group 也是按这套 CoE 映射建立的 IOmap，
         因此按实际映射计算偏移才能保证写对位置。 */
      if (sl->mbx_l > 0 && (sl->mbx_proto & ECT_MBXPROT_COE))
      {
         nactual = tc3_coe_read_pdos(context, (uint16)i, 0,
                                     actual, TC3_MAX_PDO / 2);
         nactual += tc3_coe_read_pdos(context, (uint16)i, 1,
                                      actual + nactual, TC3_MAX_PDO - nactual);
      }

      tc3_slave_io_base_bits(context, sl, &bit_out, &bit_in);   /* 关键：Ooffset/Ioffset 恒为 0，必须按指针差算基址 */

      if (nactual > 0)
      {
         for (t = 0; t < nactual; t++)
         {
            const tc3_pdo_t *p = &actual[t];
            int bit = (p->dir == 0) ? bit_out : bit_in;

            for (j = 0; j < p->entry_count && list->count < TC3_MAX_VARS; j++)
            {
               const tc3_entry_t *e = have_esi ?
                  tc3_esi_find_entry(&esi, p->entry[j].index,
                                     p->entry[j].subindex) : NULL;
               tc3_variable_t *v = &list->var[list->count];

               memset(v, 0, sizeof(*v));
               if (e != NULL && e->name[0])
                  tc3_copy_name(v->name, sizeof(v->name), e->name);
               else
                  snprintf(v->name, sizeof(v->name), "0x%04X:%02X",
                           p->entry[j].index, p->entry[j].subindex);
               v->index = p->entry[j].index;
               v->sub = p->entry[j].subindex;
               v->bitlen = p->entry[j].bitlen;
               v->dir = p->dir;
               v->slave = (uint8)i;
               v->offset = (uint32)(bit / 8);
               v->bitoff = (uint8)(bit % 8);
               if (e != NULL && e->type[0])
                  tc3_copy_name(v->type, sizeof(v->type), e->type);
               else
                  snprintf(v->type, sizeof(v->type), "CoE");
               list->count++;
               bit += p->entry[j].bitlen;
            }
            if (p->dir == 0)
               bit_out = bit;
            else
               bit_in = bit;
         }
      }
      else if (have_esi)
      {
         int bo, bi;
         /* 无邮箱/CoE 读取失败：回退 ESI 布局（SII 从站与 ESI 天然一致） */
         tc3_slave_io_base_bits(context, sl, &bo, &bi);
         tc3_fill_varlist_slave(list, i, &esi, bo, bi);
      }
   }
   return list->count;
}

/* ---------- ESI 匹配检查（供实时界面显示警告） ---------- */

/*
 * 检查所有从站与 xml_dir 下 ESI 的匹配情况，两类问题都算不匹配：
 *   1. 身份（厂商 ID + 产品代码）在 xml/ 中找不到对应 <Device>；
 *   2. ESI 的 PDO 定义总位数与从站实际映射（SM2/SM3 长度）不一致
 *      ——说明 XML 与从站固件版本不对应。
 * 返回不匹配的从站数量（0 = 全部匹配）；msg 填一行摘要，全匹配时为空串。
 */
int tc3_esi_mismatch_summary(ecx_contextt *context, const char *xml_dir,
                             char *msg, size_t msz)
{
   int i, n, bad = 0;

   if (msg != NULL && msz > 0)
      msg[0] = '\0';
   if (context == NULL)
      return 0;

   n = context->slavecount;
   for (i = 1; i <= n; i++)
   {
      ec_slavet *sl = &context->slavelist[i];
      tc3_device_t dev;
      long esi_out_bits, esi_in_bits;

      if (tc3_find_device_in_dir(xml_dir, sl->eep_man, sl->eep_id, &dev,
                                 NULL, 0) != 0)
      {
         bad++;
         if (msg != NULL && msz > 0)
         {
            snprintf(msg, msz,
                     TR("%d/%d 个从站未匹配到 xml/ 中的 ESI（厂商ID/产品代码不符），"
                        "变量按从站实际映射显示",
                        "%d/%d slaves did not match any ESI in xml/"
                        " (vendor ID/product code mismatch),"
                        " variables shown per actual slave mapping"),
                     bad, n);
         }
         continue;
      }

      /* PDO 定义与实际映射比对（跳过 0x0000 填充项） */
      tc3_esi_total_bits(&dev, &esi_out_bits, &esi_in_bits);
      if (esi_out_bits != (long)sl->Obytes * 8 ||
          esi_in_bits != (long)sl->Ibytes * 8)
      {
         bad++;
         if (msg != NULL && msz > 0)
         {
            snprintf(msg, msz,
                     TR("ESI PDO 定义与从站固件实际映射不符"
                        "（输出 ESI %ldbit / 实际 %uB；输入 ESI %ldbit / 实际 %uB），"
                        "请更新 XML 使其与固件一致",
                        "ESI PDO definition does not match the slave firmware"
                        " (output ESI %ldbit / actual %uB; input ESI %ldbit /"
                        " actual %uB), update the XML to match the firmware"),
                     esi_out_bits, sl->Obytes, esi_in_bits, sl->Ibytes);
         }
      }
   }
   return bad;
}
