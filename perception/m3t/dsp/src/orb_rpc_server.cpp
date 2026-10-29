// SPDX-License-Identifier: MIT
#include "rbt_m3t_orb.h"

#include "rbt_orb_protocol.h"

#include <stdint.h>

namespace {

alignas(128) uint8_t g_workspace[RBT_ORB_WORKSPACE_CAPACITY];
uint32_t g_workspace_busy = 0u;

bool try_acquire_workspace() {
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&g_workspace_busy, &expected, 1u,
                                       false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

void release_workspace() {
    __atomic_store_n(&g_workspace_busy, 0u, __ATOMIC_RELEASE);
}

}  // namespace

extern "C" int rbt_m3t_orb_open(const char* uri, remote_handle64* handle) {
    if (uri == nullptr || handle == nullptr) return AEE_EBADPARM;
    *handle = 1u;
    return AEE_SUCCESS;
}

extern "C" int rbt_m3t_orb_close(remote_handle64 handle) {
    return handle == 1u ? AEE_SUCCESS : AEE_EBADPARM;
}

extern "C" int rbt_m3t_orb_execute(remote_handle64 handle,
                                    const unsigned char* request,
                                    int request_length,
                                    unsigned char* response,
                                    int response_capacity) {
    if (handle != 1u || request == nullptr || response == nullptr ||
        request_length < 0 ||
        response_capacity < static_cast<int>(sizeof(RbtOrbResponse))) {
        return AEE_EBADPARM;
    }

    if (!try_acquire_workspace()) return AEE_EBUSY;

    (void)rbt_orb_dispatch(request, static_cast<uint32_t>(request_length),
                           response,
                           static_cast<uint32_t>(response_capacity),
                           g_workspace, sizeof(g_workspace));
    release_workspace();
    return AEE_SUCCESS;
}
