// SPDX-License-Identifier: MIT
#ifndef RBT_M3T_DSP_ORB_PROTOCOL_H
#define RBT_M3T_DSP_ORB_PROTOCOL_H

#include "rbt_orb_core.h"

// Private, versioned FastRPC payload ABI. Both supported processors are
// little-endian. This is independent of RBT Stream/IDL message layouts.
#define RBT_ORB_REQUEST_MAGIC 0x514f4252u
#define RBT_ORB_RESPONSE_MAGIC 0x524f4252u
#define RBT_ORB_PROTOCOL_VERSION 1u
#define RBT_ORB_IMPLEMENTATION_VERSION 1u
#define RBT_ORB_REQUIRED_FEATURES 0x7fu

typedef enum RbtOrbOperation {
    RBT_ORB_CAPABILITIES = 1,
    RBT_ORB_EXTRACT = 2,
    RBT_ORB_MATCH = 3
} RbtOrbOperation;

typedef struct RbtOrbRequest {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t request_bytes;
    uint32_t response_capacity;
    uint32_t reserved[3];
    union {
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t stride;
            RbtOrbConfig config;
            uint32_t image_bytes;
        } extract;
        struct {
            uint32_t query_count;
            uint32_t train_count;
            uint32_t reserved[6];
        } match;
        uint32_t words[8];
    } args;
} RbtOrbRequest;

typedef struct RbtOrbResponse {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t response_bytes;
    int32_t status;
    uint32_t count;
    uint32_t implemented_features;
    uint32_t reserved;
} RbtOrbResponse;

typedef struct RbtOrbCapabilities {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_features;
    uint32_t max_levels;
    uint32_t workspace_capacity;
    uint32_t implementation_version;
    uint32_t reserved[2];
} RbtOrbCapabilities;

// EXTRACT request: header followed by exactly stride*height image bytes.
// EXTRACT response: header, n_features keypoint slots, then n_features*32
// descriptor bytes. count slots are populated; unused slots are zeroed.
// MATCH request: header followed by packed query rows, then packed train rows.
// MATCH response: header followed by query_count RbtOrbMatch records.
// CAPABILITIES request has no payload; response has one capabilities record.
// Parsers check all sizes, reserved fields, counts, and response capacity.

#ifdef __cplusplus
static_assert(sizeof(RbtOrbConfig) == 16, "ORB config ABI");
static_assert(sizeof(RbtOrbKeypoint) == 24, "ORB keypoint ABI");
static_assert(sizeof(RbtOrbMatch) == 16, "ORB match ABI");
static_assert(sizeof(RbtOrbRequest) == 64, "ORB request ABI");
static_assert(sizeof(RbtOrbResponse) == 32, "ORB response ABI");
static_assert(sizeof(RbtOrbCapabilities) == 32, "ORB capabilities ABI");
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Pure dispatcher shared by native reference tests and the DSP skeleton.
// Caller supplies a 128-byte-aligned workspace. No allocation or global state.
int rbt_orb_dispatch(const uint8_t* request, uint32_t request_bytes,
                      uint8_t* response, uint32_t response_capacity,
                      void* workspace, size_t workspace_bytes);
#ifdef __cplusplus
}
#endif
#endif
