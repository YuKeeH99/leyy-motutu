#include <assert.h>
#include <stdbool.h>
#include "lingxin_identity_priority.h"

int main(void)
{
    const lingxin_identity_bundle_t primary = {
        "primary-app", "primary-license", "primary-sn", "primary-code", "primary-device"
    };
    const lingxin_identity_bundle_t incomplete = {
        "partial-app", "", "partial-sn", "partial-code", "partial-device"
    };

    assert(lingxin_identity_bundle_complete(&primary));
    assert(!lingxin_identity_bundle_complete(&incomplete));
    assert(lingxin_identity_bundle_source(true, &primary) ==
           LINGXIN_IDENTITY_SOURCE_FACTORY);
    assert(lingxin_identity_bundle_source(true, &incomplete) ==
           LINGXIN_IDENTITY_SOURCE_MISSING);
    assert(lingxin_identity_bundle_source(false, &primary) ==
           LINGXIN_IDENTITY_SOURCE_MISSING);
    return 0;
}
