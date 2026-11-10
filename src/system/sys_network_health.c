#include "sys_network_health.h"

#ifdef CONFIG_VOICE_LINK_LINGXIN
#include <stdio.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>
#include "lingxin_server_config.h"
#include "sys_network_manager.h"

bool sys_network_business_reachable(void)
{
    /* This fallback tests the Wi-Fi uplink. Keep every descriptor operation
     * in lwIP: generic socket() returns a SAL descriptor, fcntl() is a nosys
     * stub, and close() closes an unrelated LVFS file on this platform.
     * SAL select() also does not translate descriptors to the lwIP table.
     * A modem must use its own transport probe, never the Wi-Fi default route. */
    sys_network_status_t status = {0};
    if (sys_network_get_status(&status) != 0 || status.switching ||
        status.active_bearer != SYS_NETWORK_BEARER_WIFI || !status.wifi_connected)
        return false;

    struct addrinfo hints = {0}, *addresses = NULL;
    char port[8];
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", lingxin_server_config_cloud_port());
    if (lwip_getaddrinfo(lingxin_server_config_cloud_host(), port, &hints, &addresses) != 0)
        return false;

    bool reachable = false;
    int fd = lwip_socket(addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    int nonblocking = 1;
    if (fd >= 0 && fd < FD_SETSIZE && lwip_ioctl(fd, FIONBIO, &nonblocking) == 0) {
        int result = lwip_connect(fd, addresses->ai_addr, addresses->ai_addrlen);
        if (result == 0) {
            reachable = true;
        } else if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
            int error = -1;
            socklen_t size = sizeof(error);
            if (lwip_select(fd + 1, NULL, &writable, NULL, &timeout) > 0 &&
                lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)
                reachable = true;
        }
    }
    if (fd >= 0) lwip_close(fd);
    lwip_freeaddrinfo(addresses);
    return reachable;
}
#else
bool sys_network_business_reachable(void) { return false; }
#endif
