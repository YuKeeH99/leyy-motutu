#include "lingxin_identity_priority.h"

#include <stddef.h>

static int identity_present(const char *value)
{
    return value != NULL && value[0] != '\0';
}

const char *lingxin_identity_source_name(lingxin_identity_source_t source)
{
    switch (source) {
    case LINGXIN_IDENTITY_SOURCE_FACTORY:
        return "factory-kv";
    case LINGXIN_IDENTITY_SOURCE_MISSING:
    default:
        return "missing";
    }
}

int lingxin_identity_bundle_complete(const lingxin_identity_bundle_t *bundle)
{
    return bundle != NULL &&
           identity_present(bundle->app_id) &&
           identity_present(bundle->license) &&
           identity_present(bundle->sn) &&
           identity_present(bundle->app_code) &&
           identity_present(bundle->device_code);
}

lingxin_identity_source_t lingxin_identity_bundle_source(
    int factory_provisioned,
    const lingxin_identity_bundle_t *factory_bundle)
{
    return factory_provisioned && lingxin_identity_bundle_complete(factory_bundle)
               ? LINGXIN_IDENTITY_SOURCE_FACTORY
               : LINGXIN_IDENTITY_SOURCE_MISSING;
}
