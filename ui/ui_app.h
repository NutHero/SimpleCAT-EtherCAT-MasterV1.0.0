/*
 * ui_app.h - SimpleCAT 图形界面入口
 */
#ifndef _ui_app_
#define _ui_app_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 运行图形界面（阻塞直到窗口关闭）
 * 无参数启动 soem_io.exe 时由 main() 调用；进入前会释放控制台。
 * @return 消息循环退出码
 */
int ui_app_run(void);

#ifdef __cplusplus
}
#endif

#endif /* _ui_app_ */
