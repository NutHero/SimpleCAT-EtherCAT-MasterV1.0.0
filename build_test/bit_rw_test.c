/* bit_rw_test.c -- 独立验证子字节(BOOL)变量的 read/write_var_value 逻辑
   （函数体与 src/master_core.c 保持一致） */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct {
   char name[64];
   char type[16];
   uint32_t offset;
   uint8_t bitoff;
   int bitlen;
   int dir;
   uint8_t slave;
   uint16_t index;
   uint8_t sub;
} tc3_variable_t;

static int var_is_signed(const tc3_variable_t *v)
{
   return strcmp(v->type, "INT") == 0 ||
          strcmp(v->type, "SINT") == 0 ||
          strcmp(v->type, "DINT") == 0;
}

static long long read_var_value(const tc3_variable_t *v, const uint8_t *base)
{
   switch (v->bitlen)
   {
      case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      {
         uint8_t mask = (uint8_t)((0xFFu >> (8 - v->bitlen)) << v->bitoff);
         return (long long)((base[v->offset] & mask) >> v->bitoff);
      }
      case 8:
         return var_is_signed(v) ? (long long)(int8_t)base[v->offset]
                                 : (long long)base[v->offset];
      case 16:
      {
         uint16_t u = 0;
         memcpy(&u, base + v->offset, 2);
         return var_is_signed(v) ? (long long)(int16_t)u : (long long)u;
      }
      case 32:
      {
         uint32_t u = 0;
         memcpy(&u, base + v->offset, 4);
         return var_is_signed(v) ? (long long)(int32_t)u : (long long)u;
      }
      default:
         return 0;
   }
}

static void write_var_value(const tc3_variable_t *v, uint8_t *outputs, long value)
{
   switch (v->bitlen)
   {
      case 1: case 2: case 3: case 4: case 5: case 6: case 7:
      {
         uint8_t mask = (uint8_t)((0xFFu >> (8 - v->bitlen)) << v->bitoff);
         uint8_t cur = outputs[v->offset];
         cur = (uint8_t)((cur & (uint8_t)(~mask)) |
                         (uint8_t)(((uint8_t)value << v->bitoff) & mask));
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
         break;
   }
}

int main(void)
{
   tc3_variable_t v;
   uint8_t buf[4];
   long long r;

   strcpy(v.name, "M1_EN");
   strcpy(v.type, "BOOL");
   v.bitlen = 1; v.offset = 2; v.bitoff = 5; v.dir = 0; v.slave = 0;
   v.index = 0x7010; v.sub = 1;

   memset(buf, 0, sizeof(buf));
   write_var_value(&v, buf, 1);
   r = read_var_value(&v, buf);
   printf("set bit=1  -> read=%lld byte=0x%02X\n", r, buf[2]);
   if (r != 1) { printf("FAIL A\n"); return 1; }

   buf[2] = 0x01;   /* 同字节 bit0 已被别的变量占用 */
   write_var_value(&v, buf, 1);
   r = read_var_value(&v, buf);
   printf("keep bit0  -> read=%lld byte=0x%02X\n", r, buf[2]);
   if (r != 1 || buf[2] != 0x21) { printf("FAIL B\n"); return 1; }

   write_var_value(&v, buf, 0);
   r = read_var_value(&v, buf);
   printf("clear      -> read=%lld byte=0x%02X\n", r, buf[2]);
   if (r != 0 || buf[2] != 0x01) { printf("FAIL C\n"); return 1; }

   v.bitlen = 2; v.bitoff = 6;
   write_var_value(&v, buf, 2);
   r = read_var_value(&v, buf);
   printf("2bit val=2 -> read=%lld byte=0x%02X\n", r, buf[2]);
   if (r != 2 || (buf[2] & 0x01) != 0x01) { printf("FAIL D\n"); return 1; }

   printf("ALL PASS\n");
   return 0;
}
