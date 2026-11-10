/**
 * Copyright (c) 2025, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

unsigned int xTaskGetTickCount(void);

void wifi_mgr_test_enter_critical(void);
void wifi_mgr_test_exit_critical(void);
#define taskENTER_CRITICAL() wifi_mgr_test_enter_critical()
#define taskEXIT_CRITICAL() wifi_mgr_test_exit_critical()

#ifdef __cplusplus
}
#endif
