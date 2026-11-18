// Diablo III: the system library applets it can open (controller setup, user picker, error and web pages, eShop).
// On the Switch these launch a separate applet; our am service runs none, so the game's thread waited forever for
// one to finish (seen 2026-11-14: nn::hid::ShowControllerStrapGuide at boot). They are answered here at once:
// the controller setup counts as done, the user picker picks the one local user, and anything online is cancelled.
#include <string.h>

#include "../hle.h"

extern const uint8_t g_user_id[16];  // runtime/services/acc.c

#define RESULT_CANCELLED ((10u << 9) | 128u)  // a library applet result for "closed without a choice"

HLE_FUNC("_ZN2nn3hid24ShowControllerStrapGuideEv", d3_show_strap_guide) {
    LOG("[applet] controller strap guide: skipped\n");
    c->x[0] = 0;
}

// nn::hid::ShowControllerSupport(const ControllerSupportArg &) -> Result: the controllers as they are are fine
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", d3_show_controller_support) {
    LOG("[applet] controller support: accepted\n");
    c->x[0] = 0;
}

// nn::account::ShowUserSelector(Uid *out, const UserSelectionSettings &) -> Result: the one local user
HLE_FUNC("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", d3_show_user_selector) {
    if (c->x[0]) memcpy((void *)(uintptr_t)c->x[0], g_user_id, 16);
    LOG("[applet] user selector: local user\n");
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", d3_show_license) {
    LOG("[applet] network license requirements: cancelled (offline)\n");
    c->x[0] = RESULT_CANCELLED;
}

HLE_FUNC("_ZN2nn3err9ShowErrorENS_6ResultE", d3_show_error) {
    LOG("[applet] error viewer: result 0x%x (not shown)\n", (uint32_t)c->x[0]);
}

// nn::web::ShowWebPage(WebPageReturnValue *, const ShowWebPageArg &) -> Result
HLE_FUNC("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", d3_show_web_page) {
    LOG("[applet] web page: cancelled (offline)\n");
    c->x[0] = RESULT_CANCELLED;
}

HLE_FUNC("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
         d3_show_user_detail) {
    LOG("[applet] friend profile: skipped (offline)\n");
}

HLE_FUNC("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", d3_show_shop) {
    LOG("[applet] eShop add-on list: skipped (offline)\n");
}
