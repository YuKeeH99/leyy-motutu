#ifndef LINGXIN_LOG_H
#define LINGXIN_LOG_H

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * 
     * 灵芯log模块
     * ！！！！！！！！！！log 方法支持的最长参数为512字节 ！！！！！！！！！！
     * log中自动添加当前文件名和代码行，无需手动设置
     * log格式：[time] [version] [level] [module:line] [node]: log内容
     */

    /** log级别 */
#define LINGXIN_DEBUG (1 << 0)
#define LINGXIN_WARN (1 << 1)
#define LINGXIN_ERROR (1 << 2)

    /*    下面四种方法，仅打印log不缓存     */
#if defined(CONFIG_LINGXIN_VERBOSE_LOG) && CONFIG_LINGXIN_VERBOSE_LOG
#define LINGXIN_LOG_ENABLED(level) 1
#else
#define LINGXIN_LOG_ENABLED(level) ((level) != LINGXIN_DEBUG)
#endif

#define LINGXIN_LOG_PRINT(level, is_ut, node, format, ...) do { \
    int lingxin_log_level_ = (level); \
    if (LINGXIN_LOG_ENABLED(lingxin_log_level_)) { \
        _lingxin_log_print_internal_(lingxin_log_level_, is_ut, __FILE__, __LINE__, node, format, ##__VA_ARGS__); \
    } \
} while (0)

#define lingxin_log_debug(format, ...) LINGXIN_LOG_PRINT(LINGXIN_DEBUG, 0, NULL, format, ##__VA_ARGS__)
#define lingxin_log_warn(format, ...) _lingxin_log_print_internal_(LINGXIN_WARN, 0, __FILE__, __LINE__, NULL, format, ##__VA_ARGS__)
#define lingxin_log_error(format, ...) _lingxin_log_print_internal_(LINGXIN_ERROR, 0, __FILE__, __LINE__, NULL, format, ##__VA_ARGS__)

/**
 * log_level参数使用上面的log级别
 * 该方法log打印并缓存上传云端
 *  */
#define lingxin_log_ut(log_level, node_name) LINGXIN_LOG_PRINT(log_level, 1, node_name, "")
#define lingxin_log_ut_with_args(log_level, node_name, format, ...) LINGXIN_LOG_PRINT(log_level, 1, node_name, format, ##__VA_ARGS__)

    // 内部方法，勿用
    void _lingxin_log_print_internal_(int log_level, int is_ut, const char *file_path, int line, const char *node_name, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif // LINGXIN_LOG_H
