/* tc3_slave_io_base_bits 逻辑验证：
   模拟 SOEM 普通/overlapping 两种 IOmap 布局下双从站的指针关系，
   验证"指针差"基址能正确区分两个从站（旧代码用 Ooffset=0 则不能）。 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t uint8;

typedef struct { uint8 *outputs; uint8 *inputs; int Ostartbit, Istartbit; int Obytes, Ibytes; } slave_t;
typedef struct { uint8 *outputs; uint8 *inputs; } group_t;

static void base_bits(const group_t *grp, const slave_t *sl, int *ob, int *ib)
{
   int ro = 0, ri = 0;
   if (sl->outputs && grp->outputs) ro = (int)(sl->outputs - grp->outputs);
   if (sl->inputs  && grp->inputs)  ri = (int)(sl->inputs  - grp->inputs);
   *ob = ro * 8 + sl->Ostartbit;
   *ib = ri * 8 + sl->Istartbit;
}

int main(void)
{
   static uint8 iomap[4096];
   group_t grp;
   slave_t s[2];
   int ob, ib;

   /* ---- 布局1：普通映射（输出区在前，输入区跟随） ----
      slave1: O=8B I=4B  slave2: O=4B I=8B
      IOmap: [out1 8B][out2 4B][in1 4B][in2 8B] */
   memset(iomap, 0, sizeof(iomap));
   grp.outputs = iomap;
   grp.inputs = iomap + 12;
   s[0].outputs = iomap;      s[0].inputs = iomap + 12;
   s[1].outputs = iomap + 8;  s[1].inputs = iomap + 16;
   s[0].Ostartbit = s[0].Istartbit = 0;
   s[1].Ostartbit = s[1].Istartbit = 0;
   base_bits(&grp, &s[0], &ob, &ib);
   printf("normal slave1: out_bit=%d in_bit=%d (expect 0,0)\n", ob, ib);
   if (ob != 0 || ib != 0) return 1;
   base_bits(&grp, &s[1], &ob, &ib);
   printf("normal slave2: out_bit=%d in_bit=%d (expect 64,32)\n", ob, ib);
   if (ob != 64 || ib != 32) return 1;

   /* ---- 布局2：overlapping（每从站输出/输入共用逻辑段，输入整体后移 Obytes）
      stride1=max(8,4)=8, stride2=max(4,8)=8, Obytes=16
      输入区: [in1 4B][pad][in2 8B] 从 pIOmap+16 开始 */
   memset(iomap, 0, sizeof(iomap));
   grp.outputs = iomap;
   grp.inputs = iomap + 16;
   s[0].outputs = iomap;       s[0].inputs = iomap + 16;      /* +0 后移 Obytes */
   s[1].outputs = iomap + 8;   s[1].inputs = iomap + 16 + 8;  /* +stride1 后移 Obytes */
   base_bits(&grp, &s[0], &ob, &ib);
   printf("overlap slave1: out_bit=%d in_bit=%d (expect 0,0)\n", ob, ib);
   if (ob != 0 || ib != 0) return 1;
   base_bits(&grp, &s[1], &ob, &ib);
   printf("overlap slave2: out_bit=%d in_bit=%d (expect 64,64)\n", ob, ib);
   if (ob != 64 || ib != 64) return 1;

   printf("ALL PASS\n");
   return 0;
}
