#include "hos.h"
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

/* nifm under HOS_FORCE_ONLINE=1 reports a connected network on 127.0.0.1; without it the game stays offline. */
int hos_force_online(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HOS_FORCE_ONLINE"); v = e && *e == '1'; }
    return v;
}

HLE(_ZN2nn4nifm18IsNetworkAvailableEv) { HLE_RETURN(hos_force_online() ? 1 : 0); }
HLE(_ZN2nn4nifm22IsNetworkRequestOnHoldEv) { HLE_RETURN(0); }
HLE(_ZN2nn4nifm20SubmitNetworkRequestEv) { HLE_RETURN(0); }
HLE(_ZN2nn4nifm31HandleNetworkRequestErrorResultEv) { HLE_RETURN(0); }
HLE(_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE) {
    if (hos_force_online()) {
        uint64_t out = ARG(0);
        if (out) *(uint32_t *)GP(out) = htonl(INADDR_LOOPBACK);
        HLE_RETURN(0);
    }
    HLE_RETURN(0x2307c);
}
HLE(_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE) {
    if (hos_force_online()) {
        uint64_t out = ARG(0);
        if (out) {
            /* sizeof(nn::nifm::ProxySetting) is 0xAA; writing more zeroes the caller's stack */
            memset(GP(out), 0, 0xAA);
        }
        HLE_RETURN(0);
    }
    HLE_RETURN(0x2307c);
}
