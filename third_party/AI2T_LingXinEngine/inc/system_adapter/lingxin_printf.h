#ifndef __LINGXIN_PRINTF_H__
#define __LINGXIN_PRINTF_H__

#ifdef __cplusplus
extern "C" {
#endif


/**
 * 打印方法
 * @param message 打印信息
 */
void lingxin_printf(char* message);

/* Preserve SDK warning/error severity at the platform log backend. */
void lingxin_log_printf(int level, char *message);

#ifdef __cplusplus
}
#endif

#endif /* __LINGXIN_PRINTF_H__ */
