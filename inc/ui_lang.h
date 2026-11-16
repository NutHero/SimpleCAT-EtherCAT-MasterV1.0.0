/*
 * ui_lang.h - 界面语言切换支持
 *
 * ui_lang: 0 = 中文, 1 = English（定义在 ui_master.c，配置存 SimpleCAT.ini）
 * TR(中文, English)：按当前语言选择字符串。两种语言都要求 UTF-8 源码。
 */
#ifndef _ui_lang_
#define _ui_lang_

#ifdef __cplusplus
extern "C" {
#endif

extern int ui_lang;

#define TR(zh, en)  ((ui_lang) ? (en) : (zh))

#ifdef __cplusplus
}
#endif

#endif /* _ui_lang_ */
