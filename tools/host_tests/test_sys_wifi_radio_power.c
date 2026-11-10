#include <assert.h>
#include <errno.h>
#include <string.h>
#include "sys_wifi_radio_power.h"

static sys_wifi_radio_power_t power;
static char calls[128];
static unsigned count;
static char fail_at;
static const sys_wifi_radio_power_ops_t ops;

static int record(char call)
{
    assert(sys_wifi_radio_power_blocked(&power));
    calls[count++] = call;
    /* A wake while an off/on operation runs must not overlap that operation. */
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == -EBUSY);
    return call == fail_at ? -EIO : 0;
}
static int pause_io(void *arg) { (void)arg; return record('P'); }
static int off(void *arg) { (void)arg; return record('F'); }
static int on(void *arg) { (void)arg; return record('N'); }
static int restore(void *arg) { (void)arg; return record('R'); }
static void release(void *arg) { (void)arg; assert(record('U') == 0); }
static const sys_wifi_radio_power_ops_t ops = {pause_io, off, on, restore, release};

static void reset(void)
{
    memset(&power, 0, sizeof(power));
    memset(calls, 0, sizeof(calls));
    count = 0;
    fail_at = 0;
}

int main(void)
{
    reset();
    assert(!sys_wifi_radio_power_blocked(&power));
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == 0);
    assert(count == 0);
    for (int i = 0; i < 8; ++i) {
        memset(calls, 0, sizeof(calls)); count = 0;
        assert(sys_wifi_radio_power_suspend(&power, &ops, NULL) == 0);
        assert(sys_wifi_radio_power_is_off(&power));
        assert(sys_wifi_radio_power_suspend(&power, &ops, NULL) == 0);
        assert(strcmp(calls, "PF") == 0);
        assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == 0);
        assert(strcmp(calls, "PFNRU") == 0);
        assert(!sys_wifi_radio_power_blocked(&power));
    }

    reset(); fail_at = 'P';
    assert(sys_wifi_radio_power_suspend(&power, &ops, NULL) == -EIO);
    assert(strcmp(calls, "P") == 0); /* Never switch power during in-flight IO. */
    assert(sys_wifi_radio_power_blocked(&power));
    fail_at = 0;
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == 0);
    assert(strcmp(calls, "PRU") == 0); /* No wifi_on if never turned off. */

    reset(); fail_at = 'F';
    assert(sys_wifi_radio_power_suspend(&power, &ops, NULL) == -EIO);
    fail_at = 0;
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == 0);
    assert(strcmp(calls, "PFRU") == 0);

    reset();
    assert(sys_wifi_radio_power_suspend(&power, &ops, NULL) == 0);
    fail_at = 'N';
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == -EIO);
    assert(sys_wifi_radio_power_is_off(&power));
    assert(strcmp(calls, "PFN") == 0);
    fail_at = 'R';
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == -EIO);
    assert(!sys_wifi_radio_power_is_off(&power));
    assert(sys_wifi_radio_power_blocked(&power));
    fail_at = 0;
    assert(sys_wifi_radio_power_resume(&power, &ops, NULL) == 0);
    assert(strcmp(calls, "PFNNRRU") == 0); /* Retry restore, not wifi_on again. */
    assert(!sys_wifi_radio_power_blocked(&power));
    return 0;
}
