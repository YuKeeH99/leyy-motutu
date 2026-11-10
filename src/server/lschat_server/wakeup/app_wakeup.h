#pragma once

#include <stdbool.h>
#include <stdint.h>
bool app_wakeup_product_ready(void);

struct wakeup_algo_res {
    uint8_t *addr;
    uint32_t size;
};

struct wakeup_algo_resources {
    struct wakeup_algo_res mlp;
    struct wakeup_algo_res wrap;
};

typedef enum{
    APP_WAKEUP_SENSITIVITY_LEVEL_0 = 0, /*极易唤醒*/
    APP_WAKEUP_SENSITIVITY_LEVEL_1,     /*易唤醒*/
    APP_WAKEUP_SENSITIVITY_LEVEL_2,      /*默认档位*/
    APP_WAKEUP_SENSITIVITY_LEVEL_3,      /*难唤醒*/
    APP_WAKEUP_SENSITIVITY_LEVEL_4,      /*极难唤醒*/
}app_wakeup_sensitivity_level_e;

typedef enum {
    APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN = 0,
    APP_AUDIO_LOCALIZATION_SIDE_MIC0,
    APP_AUDIO_LOCALIZATION_SIDE_CENTER,
    APP_AUDIO_LOCALIZATION_SIDE_MIC1,
} app_audio_localization_side_e;

/**
 * Read-only raw dual-microphone localization result.
 *
 * axis_angle_deg is an array-axis angle, not a 360-degree world heading.
 * Positive delay means MIC0 received the sound first; negative means MIC1.
 */
typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    int32_t delay_milli_samples;
    int16_t axis_angle_deg;
    uint16_t correlation_milli;
    uint16_t active_blocks;
    uint16_t total_blocks;
    uint16_t mic0_mean_abs;
    uint16_t mic1_mean_abs;
    uint16_t mic0_peak;
    uint16_t mic1_peak;
    int16_t level_balance_milli;
    app_audio_localization_side_e side;
    bool valid;
    bool echo_gated;
    bool level_fused;
} app_audio_localization_snapshot_t;

int app_wakeup_init(struct wakeup_algo_resources *res);

#ifdef CONFIG_BOARD_ARCS_MINI
int app_wakeup_stop(void);
#endif

/**
 * @brief 设置唤醒灵敏度级别
 * @param level 灵敏度级别: LEVEL_0(低), LEVEL_1(中), LEVEL_2(高)
 * @return 0: 成功, 其他值: 失败
 */
int app_wakeup_sensitivity_level_set(app_wakeup_sensitivity_level_e level);

/**
 * @brief 获取当前唤醒灵敏度级别
 * @return 当前灵敏度级别
 */
app_wakeup_sensitivity_level_e app_wakeup_sensitivity_level_get(void);

/**
 * Copy the latest TDOA result without performing audio, JSON, or shell I/O.
 * Returns false when TDOA is not compiled in or a consistent snapshot could
 * not be obtained.
 */
bool app_audio_localization_get_snapshot(app_audio_localization_snapshot_t *snapshot);
