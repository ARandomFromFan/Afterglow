#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *hos_romfs_dir;

struct BcatFile {
    uint64_t this_ptr;
    int open;
    uint8_t *data;
    size_t size;
};
static struct BcatFile s_bcat_files[8];

/* BCAT sync stub: under HOS_FORCE_ONLINE=1 reports delivery cache sync complete with no new files. */
static int force_online(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HOS_FORCE_ONLINE"); v = e && *e == '1'; }
    return v;
}

HLE(_ZN2nn4bcat10InitializeEv) {
    hos_log("bcat: Initialize");
    HLE_RETURN(0);
}

HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE) {
    uint64_t prog = ARG(0);
    hos_log("bcat: RequestSyncDeliveryCache(progress=%#llx)", (unsigned long long)prog);
    if (force_online()) {
        if (prog) {
            G32(prog + 88) = 9;
            G32(prog + 92) = 0;
        }
        HLE_RETURN(0);
    }
    HLE_RETURN(0x1e80a);
}

HLE(_ZN2nn4bcat25MountDeliveryCacheStorageEv) {
    hos_log("bcat: MountDeliveryCacheStorage");
    HLE_RETURN(force_online() ? 0 : 0x1e80a);
}

HLE(_ZN2nn4bcat27UnmountDeliveryCacheStorageEv) {
    hos_log("bcat: UnmountDeliveryCacheStorage");
    HLE_RETURN(0);
}

HLE(_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc) {
    if (!force_online()) HLE_RETURN(0x1e80a);
    uint64_t this_ptr = ARG(0);
    uint64_t arg1 = ARG(1);
    char dir[64] = {0};
    char file[64] = {0};
    const char *str1 = arg1 ? (const char *)GP(arg1) : "";
    const char *slash = strchr(str1, '/');
    if (slash) {
        size_t dlen = slash - str1;
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, str1, dlen);
        dir[dlen] = '\0';
        strncpy(file, slash + 1, sizeof(file) - 1);
    } else {
        strncpy(dir, str1, sizeof(dir) - 1);
    }
    hos_log("bcat: open raw='%s' -> %s/%s", str1, dir, file);
    if (strcmp(dir, "Regulation") != 0 || strcmp(file, "regulation_nt_1_0.bin") != 0) {
        HLE_RETURN(0x7A8C);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/regulation_nt.bin", hos_romfs_dir ? hos_romfs_dir : "romfs");
    FILE *f = fopen(path, "rb");
    if (!f) {
        hos_log("bcat: failed to read %s", path);
        HLE_RETURN(0x7A8C);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        HLE_RETURN(0x7A8C);
    }
    uint8_t *data = (uint8_t *)malloc(sz);
    if (!data) {
        fclose(f);
        HLE_RETURN(0x7A8C);
    }
    if (fread(data, 1, sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        HLE_RETURN(0x7A8C);
    }
    fclose(f);

    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (s_bcat_files[i].open && s_bcat_files[i].this_ptr == this_ptr) {
            slot = i;
            free(s_bcat_files[i].data);
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 8; i++) {
            if (!s_bcat_files[i].open) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) { slot = 0; free(s_bcat_files[0].data); }
    s_bcat_files[slot].this_ptr = this_ptr;
    s_bcat_files[slot].open = 1;
    s_bcat_files[slot].data = data;
    s_bcat_files[slot].size = (size_t)sz;
    HLE_RETURN(0);
}

HLE(_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv) {
    if (!force_online()) HLE_RETURN(0);
    uint64_t this_ptr = ARG(0);
    for (int i = 0; i < 8; i++) {
        if (s_bcat_files[i].open && s_bcat_files[i].this_ptr == this_ptr) {
            HLE_RETURN(s_bcat_files[i].size);
        }
    }
    HLE_RETURN(0);
}

HLE(_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm) {
    if (!force_online()) HLE_RETURN(0x1e80a);
    uint64_t this_ptr = ARG(0);
    uint64_t out_read = ARG(1);
    uint64_t offset = ARG(2);
    uint64_t buf = ARG(3);
    uint64_t size = ARG(4);
    for (int i = 0; i < 8; i++) {
        if (s_bcat_files[i].open && s_bcat_files[i].this_ptr == this_ptr) {
            size_t file_sz = s_bcat_files[i].size;
            size_t to_read = 0;
            if (offset < file_sz) {
                to_read = file_sz - offset;
                if (to_read > size) to_read = size;
                memcpy(GP(buf), s_bcat_files[i].data + offset, to_read);
            }
            if (out_read) G64(out_read) = to_read;
            HLE_RETURN(0);
        }
    }
    HLE_RETURN(0x7A8C);
}

HLE(_ZN2nn4bcat17DeliveryCacheFile5CloseEv) {
    uint64_t this_ptr = ARG(0);
    for (int i = 0; i < 8; i++) {
        if (s_bcat_files[i].open && s_bcat_files[i].this_ptr == this_ptr) {
            free(s_bcat_files[i].data);
            s_bcat_files[i].data = NULL;
            s_bcat_files[i].size = 0;
            s_bcat_files[i].open = 0;
            s_bcat_files[i].this_ptr = 0;
            break;
        }
    }
    HLE_RETURN(0);
}
