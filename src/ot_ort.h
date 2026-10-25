// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ONNX Runtime, loaded at run time by absolute path (`OrtGetApiBase`
 *         only), and the one inference shape the tracker needs: one NHWC
 *         float image in, every float output copied out.
 *
 * The provider never imports ONNX Runtime statically: a missing or older
 * library is a probe answer ("declined: ..."), never a load failure of the
 * provider, and a same-named library elsewhere in the process can never be
 * picked up by name.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ot_ort;
struct ot_session;

enum ot_ort_status
{
	OT_ORT_OK = 0,
	OT_ORT_ABSENT,  //!< the library is not there
	OT_ORT_TOO_OLD, //!< present, but it does not serve the API version we were built against
	OT_ORT_BROKEN,  //!< present and loadable, but not usable
};

enum ot_ep
{
	OT_EP_CPU = 0,
	OT_EP_COREML = 1, //!< macOS
};

//! Core ML compute units (MLComputeUnits).
enum ot_coreml_units
{
	OT_COREML_ALL = 0, //!< CPU + GPU + Neural Engine
	OT_COREML_GPU = 1, //!< CPU + GPU
	OT_COREML_ANE = 2, //!< CPU + Neural Engine
	OT_COREML_CPU = 3, //!< CPU only
};

struct ot_ep_config
{
	enum ot_coreml_units coreml_units;
	const char *coreml_cache_dir; //!< compiled-model cache directory; NULL = none (compile every start)
};

enum ot_ort_log
{
	OT_ORT_LOG_WARNING = 0,
	OT_ORT_LOG_INFO = 1,
	OT_ORT_LOG_VERBOSE = 2,
};

#define OT_ORT_MAX_OUTPUTS 4

//! The library file name next to the provider.
const char *
ot_ort_library_name(void);

enum ot_ort_status
ot_ort_load(const char *lib_path, enum ot_ort_log log, struct ot_ort **out, char *detail, size_t detail_size);

const char *
ot_ort_version(const struct ot_ort *ort);

bool
ot_ort_has_coreml(const struct ot_ort *ort);

/*!
 * A session for the model file at @p path (UTF-8, absolute). @p threads 0 =
 * ONNX Runtime's default. On Core ML the graph is partitioned: what Core ML
 * cannot take runs on the CPU execution provider in the same session;
 * ot_session_ep_nodes says how much Core ML took.
 */
bool
ot_session_create(struct ot_ort *ort,
                  const char *path,
                  enum ot_ep ep,
                  const struct ot_ep_config *epc,
                  int threads,
                  struct ot_session **out,
                  char *err,
                  size_t err_size);

void
ot_session_destroy(struct ot_session *s);

bool
ot_session_ep_nodes(const struct ot_session *s, uint32_t *taken, uint32_t *total, uint32_t *partitions);

//! The single input's NHWC shape (batch, height, width, channels); false unless rank 4, all fixed.
bool
ot_session_input_shape(const struct ot_session *s, int64_t out[4]);

//! Number of outputs and the element count of output @p i (all dims fixed).
uint32_t
ot_session_output_count(const struct ot_session *s);
size_t
ot_session_output_elems(const struct ot_session *s, uint32_t i);
//! The graph's name of output @p i ("" when out of range).
const char *
ot_session_output_name(const struct ot_session *s, uint32_t i);

/*!
 * Run: @p nhwc is the input (its shape is the session's), each output @p i is
 * copied into @p outs[i] (capacity @p caps[i] floats).
 */
bool
ot_session_run(struct ot_session *s, const float *nhwc, float *const *outs, const size_t *caps, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
