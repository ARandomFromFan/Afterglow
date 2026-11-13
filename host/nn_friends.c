/* nn::friends: an offline console with no friend invitations. */
#include "hos.h"

uint64_t hos_sys_alloc(uint64_t size, uint64_t align);   /* hos_mem.c */
void hos_system_event_init(uint64_t key, int autoclear);  /* nn_os_sync.c */

/* GetFriendInvitationNotificationEvent() -> os::SystemEventType*: an event that is never signalled */
HLE(_ZN2nn7friends36GetFriendInvitationNotificationEventEv) {
    static uint64_t ev;
    if (!ev) {
        ev = hos_sys_alloc(0x100, 0x10);
        hos_system_event_init(ev, 0);
    }
    RET(ev);
}

/* TryPopFriendInvitationNotificationInfo(Uid*, u64*, void*, size) -> bool: none pending */
HLE(_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm) { RET(0); }
