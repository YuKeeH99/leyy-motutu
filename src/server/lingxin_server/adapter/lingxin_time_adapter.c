#include "lingxin_system_time.h"

#include <time.h>

#include "lisa_time.h"
#include "listen_system.h"

static time_t lingxin_now_s(void)
{
    struct timeval tv;

    if (ls_sys_get_time(&tv) == 0 && tv.tv_sec > 1600000000L) {
        return (time_t)tv.tv_sec;
    }

    return (time_t)lisa_os_get_time();
}

void lingxin_get_current_time(LINGXIN_TIME *lingxin_time)
{
    if (!lingxin_time) {
        return;
    }

    time_t now = lingxin_now_s();
    struct tm tmv;
    localtime_r(&now, &tmv);

    lingxin_time->year = tmv.tm_year + 1900;
    lingxin_time->mon = tmv.tm_mon + 1;
    lingxin_time->day = tmv.tm_mday;
    lingxin_time->hour = tmv.tm_hour;
    lingxin_time->min = tmv.tm_min;
    lingxin_time->sec = tmv.tm_sec;
    lingxin_time->mill_sec = (int)(lisa_os_get_tick_ms() % 1000);
}

long lingxin_get_timestamp_s()
{
    return (long)lingxin_now_s();
}
