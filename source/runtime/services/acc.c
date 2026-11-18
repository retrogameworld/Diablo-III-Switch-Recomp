// acc:u0 (one local user) and the preselected-user launch parameter.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../settings.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// The user profile's nickname - the name the game shows under P1 on its main menu (the tester 2026-11-17: it said "Link",
// the Link's Awakening port's name): player_name in larecomp.ini, else the PC's user name, else "Player". UTF-8, at
// most 32 bytes (cut at a whole character).
static void nickname(char *out) {
    char n[64] = "";
    if (g_settings.player_name[0]) snprintf(n, sizeof n, "%s", g_settings.player_name);
#ifdef _WIN32
    else {
        wchar_t w[64];
        DWORD wn = 64;
        if (GetUserNameW(w, &wn) && wn > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, n, sizeof n, NULL, NULL);
    }
#else
    else if (getenv("USER")) snprintf(n, sizeof n, "%s", getenv("USER"));
#endif
    if (!n[0]) strcpy(n, "Player");
    size_t k = strlen(n);
    if (k > 32) {
        k = 32;
        while (k && ((unsigned char)n[k] & 0xC0) == 0x80) k--;  // (not inside a character)
    }
    memcpy(out, n, k);
    out[k] = 0;
}

const uint8_t g_user_id[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 0x01, 0, 0, 0, 0, 0, 0, 0x01};

static void write_uid_list(IpcReq *req) {
    IpcBuf *c = out_buf(req, 0);
    if (!c) return;
    memset((void *)(uintptr_t)c->addr, 0, c->size);
    if (c->size >= 16) memcpy((void *)(uintptr_t)c->addr, g_user_id, 16);
}

static void profile(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: {  // Get / GetBase -> ProfileBase {uid, u64 timestamp, nickname[0x21]}
        memset(resp->data, 0, 0x38);
        memcpy(resp->data, g_user_id, 16);
        *(uint64_t *)(resp->data + 16) = 1600000000;
        nickname((char *)resp->data + 24);
        resp_size(resp, 0x38);
        if (req->cmd == 0 && out_buf(req, 0)) memset((void *)(uintptr_t)out_buf(req, 0)->addr, 0, out_buf(req, 0)->size);
        break;
    }
    case 10: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // GetImageSize
    case 11: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;  // LoadImage
    default: LOG("[acc] IProfile cmd %u (ok)\n", req->cmd);
    }
}

static void manager(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: break;  // CheckAvailability
    case 1: OUT(uint64_t, 0) = 0x1234567890abcdefull; resp_size(resp, 8); break;  // GetAccountId
    default: LOG("[acc] IManagerForApplication cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("acc:u0", acc_u0) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int32_t, 0) = 1; resp_size(resp, 4); break;  // GetUserCount
    case 1: OUT(uint8_t, 0) = !memcmp(req->data, g_user_id, 16); resp_size(resp, 1); break;
    case 2: case 3: case 141: write_uid_list(req); break;  // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 4: case 51: memcpy(resp->data, g_user_id, 16); resp_size(resp, 16); break;  // GetLastOpenedUser
    case 5: ipc_return_object(resp, service_new("IProfile", profile, NULL)); break;
    case 50: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 100: case 140: break;  // InitializeApplicationInfo
    case 101: ipc_return_object(resp, service_new("IManagerForApplication", manager, NULL)); break;
    case 150: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsUserAccountSwitchLocked
    default: LOG("[acc] acc:u0 cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
