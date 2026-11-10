#pragma once
void radio_test_enter_critical(void);
void radio_test_exit_critical(void);
#define taskENTER_CRITICAL() radio_test_enter_critical()
#define taskEXIT_CRITICAL() radio_test_exit_critical()
