// Diablo III: network services answered as "this Switch is offline". The game only needs offline single player and
// local co-op, so every online feature should see no internet connection and carry on without it.
// Command numbers and the offline behaviour follow Ryujinx's HLE services (MIT, see third_party/ryujinx/LICENSE.txt).
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

// nifm result "no internet connection" (module 110, description 300)
#define NIFM_NO_INTERNET ((300u << 9) | 110u)

// One event object for every request (never signalled): the game makes a new request every frame and closes it, so
// each call hands out one fresh handle to the shared object instead of a new event pair (which leaked handles).
static KEvent *g_nifm_event;
RT_ON_RESET(nifm_reset) { g_nifm_event = NULL; }

static uint32_t new_readable_event(void) {
    if (!g_nifm_event) {
        uint32_t w, r;
        g_nifm_event = kernel_create_event_pair(&w, &r);
    }
    return kernel_new_handle((KObject *)g_nifm_event);
}

static void nifm_log(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[nifm] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// IRequest: a connection request that never gets accepted.
static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;   // GetRequestState: 1 = error (offline)
    case 1: resp->result = NIFM_NO_INTERNET; break;           // GetResult
    case 2:                                                   // GetSystemEventReadableHandles
        resp->copy_handles[resp->ncopy++] = new_readable_event();
        resp->copy_handles[resp->ncopy++] = new_readable_event();
        break;
    case 4: break;  // Submit (every frame: not logged)
    default: nifm_log(s, req, resp);  // Cancel, SetRequirement..., SetConnectionConfirmationOption...
    }
}

static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 1: {  // GetClientId(buffer)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        break;
    }
    case 4: ipc_return_object(resp, service_new("nifm:IRequest", nifm_request, NULL)); break;  // CreateRequest
    case 5:   // GetCurrentNetworkProfile
    case 12:  // GetCurrentIpAddress
    case 15:  // GetCurrentIpConfigInfo
    case 18:  // GetInternetConnectionStatus
        resp->result = NIFM_NO_INTERNET;
        break;
    case 21: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsAnyInternetRequestAccepted
    case 22: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsAnyForegroundRequestAccepted
    default: nifm_log(s, req, resp);
    }
}

static void nifm_static(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 4 || req->cmd == 5)  // CreateGeneralServiceOld / CreateGeneralService
        ipc_return_object(resp, service_new("nifm:IGeneralService", nifm_general, NULL));
    else
        nifm_log(s, req, resp);
}
SERVICE("nifm:u", nifm_u) { nifm_static(s, req, resp); }
SERVICE("nifm:a", nifm_a) { nifm_static(s, req, resp); }
SERVICE("nifm:s", nifm_s) { nifm_static(s, req, resp); }

// ------------------------------------------------------------------ friends: no friends, nothing to report
#define FRIENDS_QUEUE_EMPTY ((15u << 9) | 121u)  // friends result: no notification waiting

static void friends_log(Service *s, IpcReq *req, IpcResp *resp) {
    static int n;
    if (n++ < 40) LOG("[friends] %s cmd %u (ok, empty)\n", s->name, req->cmd);
    resp_size(resp, 0x10);  // (counts and flags read back as zero)
}

static void friends_service(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = new_readable_event(); break;  // GetCompletionEvent
    default: friends_log(s, req, resp);  // friend lists (0 entries), presence and play-session declarations
    }
}

static void friends_notification(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = new_readable_event(); break;  // GetEvent
    case 2: resp->result = FRIENDS_QUEUE_EMPTY; break;                       // Pop
    default: friends_log(s, req, resp);
    }
}

static void friends_root(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("friends:IFriendService", friends_service, NULL));
    else if (req->cmd == 1) ipc_return_object(resp, service_new("friends:INotificationService", friends_notification, NULL));
    else friends_log(s, req, resp);
}
SERVICE("friend:u", friend_u) { friends_root(s, req, resp); }
SERVICE("friend:v", friend_v) { friends_root(s, req, resp); }
SERVICE("friend:a", friend_a) { friends_root(s, req, resp); }

// ------------------------------------------------------------------ ldn: Switch-to-Switch wireless play is not available
// (Diablo III's local co-op on one screen doesn't need it; seen 2026-11-14: the game opened ldn:u from a menu and
// crashed on the stub's missing object)
#define LDN_NOT_AVAILABLE ((22u << 9) | 203u)

static void ldn_user(Service *s, IpcReq *req, IpcResp *resp) {
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;                    // GetState: none
    case 100: resp->copy_handles[resp->ncopy++] = new_readable_event(); break;  // AttachStateChangeEvent
    case 400: case 401: case 402: case 403: case 404:                           // Initialize / Finalize variants
        if (req->cmd == 401) break;                                             // (Finalize: fine)
        resp->result = LDN_NOT_AVAILABLE;
        break;
    default: {
        static int n;
        if (n++ < 20) LOG("[ldn] %s cmd %u: not available\n", s->name, req->cmd);
        resp->result = LDN_NOT_AVAILABLE;
    }
    }
}

static void ldn_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("ldn:IUserLocalCommunicationService", ldn_user, NULL));
    else resp->result = LDN_NOT_AVAILABLE;
}
SERVICE("ldn:u", ldn_u) { ldn_root(s, req, resp); }
