/* nn::swkbd: software keyboard HLE for text entry */
#include "hos.h"
#include <string.h>
#include <stdlib.h>

HLE(_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE) {
    uint64_t config_gva = ARG(0);
    uint32_t preset = (uint32_t)ARG(1);
    hos_log("swkbd: MakePreset(config=%#llx, preset=%u)", (unsigned long long)config_gva, preset);
}

HLE(_ZN2nn5swkbd25SetRightOptionalSymbolKeyEPNS0_14KeyboardConfigEDs) {
    hos_log("swkbd: SetRightOptionalSymbolKey(key=%u)", (unsigned int)ARG(1));
}

HLE(_ZN2nn5swkbd24SetLeftOptionalSymbolKeyEPNS0_14KeyboardConfigEDs) {
    hos_log("swkbd: SetLeftOptionalSymbolKey(key=%u)", (unsigned int)ARG(1));
}

HLE(_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb) {
    hos_log("swkbd: GetRequiredWorkBufferSize(%d) -> 0x1000", (int)ARG(0));
    RET(0x1000);
}

HLE(_ZN2nn5swkbd27GetRequiredStringBufferSizeEv) {
    hos_log("swkbd: GetRequiredStringBufferSize() -> 0x1000");
    RET(0x1000);
}

HLE(_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs) {
    hos_log("swkbd: SetInitialText(arg=%#llx, text=%#llx)", (unsigned long long)ARG(0), (unsigned long long)ARG(1));
}

HLE(_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE) {
    uint64_t str_gva = ARG(0);
    const char *name = getenv("HOS_SWKBD_TEXT");
    if (!name || !*name) {
        name = "Hunter";
    }

    if (str_gva) {
        uint64_t dest_gva = *(uint64_t *)GP(str_gva);
        uint16_t *buf = NULL;
        if (dest_gva != 0 && dest_gva != str_gva) {
            buf = (uint16_t *)GP(dest_gva);
        } else {
            buf = (uint16_t *)GP(str_gva);
        }
        size_t i = 0;
        while (name[i]) {
            buf[i] = (uint16_t)(uint8_t)name[i];
            i++;
        }
        buf[i] = 0;
    }

    hos_log("swkbd: ShowKeyboard -> \"%s\" (ResultSuccess)", name);
    RET(0);
}
