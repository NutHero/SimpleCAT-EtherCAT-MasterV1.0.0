/*
 * ui_config.h - 界面配置持久化（记忆功能）
 *
 * 把用户调好的界面/主站偏好保存到 exe 同目录的 SimpleCAT.ini，
 * 下次启动自动读取恢复。用 INI 而非注册表：绿色便携、可直接查看/删除。
 *
 * 保存内容：
 *   - 皮肤（浅色/深色/蓝灰）
 *   - 从站模式（DC / FreeRUN，未设置时按从站能力自动）
 *   - 波形采样周期（1/5/10/50/100/200/500 ms）
 *   - 波形缓冲区上限（每通道点数，1000~1000000）
 *   - 窗口位置、大小、是否最大化
 *   - 上次连接的网卡（仅用于在菜单里标记，不自动连接）
 *   - 上次烧录 XML 的目标从站序号（烧录弹窗默认选中）
 */
#ifndef _ui_config_
#define _ui_config_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
   int skin;                /* 1 = 浅色, 2 = 深色, 3 = 蓝灰 */
   int slave_mode;          /* 1 = DC, 0 = FreeRUN, -1 = 未设置（自动） */
   int sample_ms;           /* 波形采样周期：1/5/10/50/100/200/500 */
   int wave_buf;            /* 波形缓冲区上限（每通道点数，1000~1000000） */
   int win_x, win_y;        /* 窗口左上角（屏幕坐标） */
   int win_w, win_h;        /* 窗口宽高（0 = 使用默认尺寸） */
   int win_max;             /* 1 = 启动时最大化 */
   char last_adapter[256];  /* 上次使用的网卡设备名（UTF-8，空 = 无） */
   int last_flash_slave;    /* 上次烧录的目标从站序号（1 起） */
   int lang;                /* 界面语言：0 = 中文, 1 = English */
} ui_config_t;

/* 填入默认值（配置不存在或某项非法时使用） */
void ui_config_defaults(ui_config_t *c);

/* 从 ini 读取；文件不存在则保持默认值。返回 1 = 读到了已有配置，0 = 首次运行 */
int ui_config_load(ui_config_t *c);

/* 写入 ini（失败静默，不影响程序运行） */
void ui_config_save(const ui_config_t *c);

/* ini 文件的完整路径（UTF-8），便于日志/提示 */
void ui_config_path(char *out, int cb);

#ifdef __cplusplus
}
#endif

#endif /* _ui_config_ */
