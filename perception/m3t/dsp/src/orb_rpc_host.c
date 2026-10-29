/* SPDX-License-Identifier: MIT */
#include <remote.h>

int rbt_m3t_orb_prepare_cdsp(void) {
    struct remote_rpc_control_unsigned_module request = {CDSP_DOMAIN_ID, 1};
    return remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE,
                                  &request,
                                  (uint32_t)sizeof(request));
}
