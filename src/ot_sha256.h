// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SHA-256 (FIPS 180-4) for verifying model blobs against the pack.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

struct ot_sha256_ctx
{
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t fill;
};

void
ot_sha256_init(struct ot_sha256_ctx *c);
void
ot_sha256_update(struct ot_sha256_ctx *c, const void *data, size_t n);
void
ot_sha256_final_hex(struct ot_sha256_ctx *c, char out[65]);
