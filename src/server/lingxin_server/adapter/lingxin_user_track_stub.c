#include "lingxin_user_track.h"

#define TAG "lingxin.ut.stub"
#include "lisa_log.h"

bool user_track_init(char *flash_path)
{
    (void)flash_path;
    LOGI("[Lingxin] user_track stub init");
    return true;
}

void user_track_record(char *log)
{
    (void)log;
}

bool is_user_track_init()
{
    return false;
}

void core_node_record(char *node)
{
    (void)node;
}
