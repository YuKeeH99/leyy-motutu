#ifndef LINGXIN_IDENTITY_PRIORITY_H
#define LINGXIN_IDENTITY_PRIORITY_H

typedef enum {
    LINGXIN_IDENTITY_SOURCE_MISSING = 0,
    LINGXIN_IDENTITY_SOURCE_FACTORY,
} lingxin_identity_source_t;

typedef struct {
    const char *app_id;
    const char *license;
    const char *sn;
    const char *app_code;
    const char *device_code;
} lingxin_identity_bundle_t;

const char *lingxin_identity_source_name(lingxin_identity_source_t source);
int lingxin_identity_bundle_complete(const lingxin_identity_bundle_t *bundle);
lingxin_identity_source_t lingxin_identity_bundle_source(
    int factory_provisioned,
    const lingxin_identity_bundle_t *factory_bundle);

#endif
