// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ONNX Runtime glue, see ot_ort.h.
 */

#include "ot_ort.h"
#include "ot_platform.h"

#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! What ONNX Runtime said about the last Core ML partitioning (written by the log callback).
struct ot_ep_report
{
	bool valid;
	uint32_t taken, total, partitions;
};

struct ot_ort
{
	void *lib;
	const OrtApi *api;
	OrtEnv *env;
	char version[32];
	bool has_coreml;
	OrtLoggingLevel print_level;
	struct ot_ep_report report; //!< sessions are created one at a time
};

struct ot_session
{
	struct ot_ort *ort;
	OrtSession *session;
	OrtMemoryInfo *cpu_mem;
	struct ot_ep_report report;
	char *in_name;
	int64_t in_shape[4];
	bool in_shape_ok;
	uint32_t out_count;
	char *out_names[OT_ORT_MAX_OUTPUTS];
	size_t out_elems[OT_ORT_MAX_OUTPUTS];
};

const char *
ot_ort_library_name(void)
{
#if defined(_WIN32)
	return "onnxruntime.dll";
#elif defined(__APPLE__)
	return "libonnxruntime.dylib";
#else
	return "libonnxruntime.so.1";
#endif
}

static bool
number_after(const char *msg, const char *key, uint32_t *out)
{
	const char *at = strstr(msg, key);
	if (at == NULL) {
		return false;
	}
	char *end = NULL;
	const unsigned long v = strtoul(at + strlen(key), &end, 10);
	if (end == at + strlen(key)) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static void ORT_API_CALL
ort_log(void *param,
        OrtLoggingLevel severity,
        const char *category,
        const char *logid,
        const char *code_location,
        const char *message)
{
	(void)category;
	(void)logid;
	(void)code_location;
	struct ot_ort *o = (struct ot_ort *)param;
	if (message == NULL) {
		message = "";
	}
	if (o != NULL && strstr(message, "CoreMLExecutionProvider::GetCapability") != NULL) {
		struct ot_ep_report r = {0};
		r.valid = number_after(message, "number of partitions supported by CoreML: ", &r.partitions) &&
		          number_after(message, "number of nodes in the graph: ", &r.total) &&
		          number_after(message, "number of nodes supported by CoreML: ", &r.taken);
		if (r.valid) {
			o->report = r;
		}
	}
	if (o == NULL || severity >= o->print_level) {
		fprintf(stderr, "[open-tracking] onnxruntime: %s\n", message);
	}
}

//! Consumes @p st. True when there was no error.
static bool
status_ok(const OrtApi *api, OrtStatus *st, char *err, size_t err_size)
{
	if (st == NULL) {
		return true;
	}
	if (err != NULL && err_size > 0) {
		snprintf(err, err_size, "%s", api->GetErrorMessage(st));
	}
	api->ReleaseStatus(st);
	return false;
}

#define TRY(EXPR)                                                                                                      \
	do {                                                                                                           \
		if (!status_ok(api, (EXPR), err, err_size)) {                                                          \
			goto fail;                                                                                     \
		}                                                                                                      \
	} while (0)

enum ot_ort_status
ot_ort_load(const char *lib_path, enum ot_ort_log log, struct ot_ort **out, char *detail, size_t detail_size)
{
	*out = NULL;
	if (!ot_file_exists(lib_path)) {
		snprintf(detail, detail_size, "ONNX Runtime not found (%s)", lib_path != NULL ? lib_path : "-");
		return OT_ORT_ABSENT;
	}
	char err[200];
	void *lib = ot_lib_open(lib_path, err, sizeof(err));
	if (lib == NULL) {
		snprintf(detail, detail_size, "ONNX Runtime did not load: %s", err);
		return OT_ORT_BROKEN;
	}
	typedef const OrtApiBase *(ORT_API_CALL * get_base_fn)(void);
	get_base_fn get_base = (get_base_fn)ot_lib_sym(lib, "OrtGetApiBase");
	const OrtApiBase *base = get_base != NULL ? get_base() : NULL;
	if (base == NULL) {
		snprintf(detail, detail_size, "ONNX Runtime has no OrtGetApiBase");
		return OT_ORT_BROKEN; // the library stays mapped: unloading ONNX Runtime is not supported
	}
	const char *ver = base->GetVersionString != NULL ? base->GetVersionString() : "?";
	const OrtApi *api = base->GetApi(ORT_API_VERSION);
	if (api == NULL) {
		snprintf(detail, detail_size, "ONNX Runtime %s is older than the supported API %u", ver,
		         (unsigned)ORT_API_VERSION);
		return OT_ORT_TOO_OLD;
	}
	struct ot_ort *o = (struct ot_ort *)calloc(1, sizeof(*o));
	if (o == NULL) {
		snprintf(detail, detail_size, "out of memory");
		return OT_ORT_BROKEN;
	}
	o->lib = lib;
	o->api = api;
	ot_copy_str(o->version, sizeof(o->version), ver);
	o->print_level = log == OT_ORT_LOG_VERBOSE ? ORT_LOGGING_LEVEL_VERBOSE
	                 : log == OT_ORT_LOG_INFO  ? ORT_LOGGING_LEVEL_INFO
	                                           : ORT_LOGGING_LEVEL_WARNING;
	// At least INFO so the Core ML partitioning summary reaches ort_log; only print_level and up is printed.
	const OrtLoggingLevel env_level =
	    o->print_level < ORT_LOGGING_LEVEL_INFO ? o->print_level : ORT_LOGGING_LEVEL_INFO;
	OrtStatus *st = api->CreateEnvWithCustomLogger(ort_log, o, env_level, "open-tracking", &o->env);
	if (!status_ok(api, st, err, sizeof(err))) {
		snprintf(detail, detail_size, "ONNX Runtime %s: environment failed: %s", ver, err);
		free(o);
		return OT_ORT_BROKEN;
	}
	char **providers = NULL;
	int count = 0;
	if (api->GetAvailableProviders(&providers, &count) == NULL) {
		for (int i = 0; i < count; i++) {
			if (strcmp(providers[i], "CoreMLExecutionProvider") == 0) {
				o->has_coreml = true;
			}
		}
		(void)status_ok(api, api->ReleaseAvailableProviders(providers, count), NULL, 0);
	}
	snprintf(detail, detail_size, "ONNX Runtime %s%s", ver, o->has_coreml ? " (Core ML)" : "");
	*out = o;
	return OT_ORT_OK;
}

const char *
ot_ort_version(const struct ot_ort *ort)
{
	return ort != NULL ? ort->version : "";
}

bool
ot_ort_has_coreml(const struct ot_ort *ort)
{
	return ort != NULL && ort->has_coreml;
}

//! Element count of a fully fixed shape; 0 when a dim is not fixed.
static size_t
shape_elems(const OrtApi *api, const OrtTypeInfo *ti, int64_t *dims_out, size_t dims_cap, size_t *rank_out)
{
	const OrtTensorTypeAndShapeInfo *info = NULL;
	if (api->CastTypeInfoToTensorInfo(ti, &info) != NULL || info == NULL) {
		return 0;
	}
	size_t n = 0;
	int64_t dims[8] = {0};
	if (api->GetDimensionsCount(info, &n) != NULL || n > 8 || api->GetDimensions(info, dims, n) != NULL) {
		return 0;
	}
	size_t e = 1;
	for (size_t i = 0; i < n; i++) {
		if (dims[i] <= 0) {
			return 0;
		}
		e *= (size_t)dims[i];
		if (dims_out != NULL && i < dims_cap) {
			dims_out[i] = dims[i];
		}
	}
	if (rank_out != NULL) {
		*rank_out = n;
	}
	return e;
}

bool
ot_session_create(struct ot_ort *ort,
                  const char *path,
                  enum ot_ep ep,
                  const struct ot_ep_config *epc,
                  int threads,
                  struct ot_session **out,
                  char *err,
                  size_t err_size)
{
	*out = NULL;
	const OrtApi *api = ort->api;
	OrtSessionOptions *opts = NULL;
	OrtAllocator *alloc = NULL;
	struct ot_session *s = (struct ot_session *)calloc(1, sizeof(*s));
	if (s == NULL) {
		snprintf(err, err_size, "out of memory");
		return false;
	}
	s->ort = ort;
	TRY(api->CreateSessionOptions(&opts));
	TRY(api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_EXTENDED));
	TRY(api->SetInterOpNumThreads(opts, 1));
	if (threads > 0) {
		TRY(api->SetIntraOpNumThreads(opts, threads));
	}
	if (ep == OT_EP_COREML) {
#ifdef __APPLE__
		// ML Program (typed fp32 / fp16 ops, GPU and Neural Engine). The pack's graphs carry the
		// rewrite that makes every node of both models Core ML-eligible (scripts/mp_onnx.py).
		static const char *const units[] = {"ALL", "CPUAndGPU", "CPUAndNeuralEngine", "CPUOnly"};
		const char *keys[4], *vals[4];
		size_t n = 0;
		keys[n] = "ModelFormat";
		vals[n++] = "MLProgram";
		keys[n] = "MLComputeUnits";
		vals[n++] = units[(unsigned)epc->coreml_units < 4u ? (unsigned)epc->coreml_units : 0u];
		if (epc->coreml_cache_dir != NULL && epc->coreml_cache_dir[0] != '\0') {
			keys[n] = "ModelCacheDirectory";
			vals[n++] = epc->coreml_cache_dir;
		}
		TRY(api->SessionOptionsAppendExecutionProvider(opts, "CoreML", keys, vals, n));
#else
		(void)epc;
		snprintf(err, err_size, "Core ML is macOS-only");
		goto fail;
#endif
	}
	TRY(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &s->cpu_mem));
#ifdef _WIN32
	{
		wchar_t wpath[1024];
		if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) {
			snprintf(err, err_size, "bad model path");
			goto fail;
		}
		TRY(api->CreateSession(ort->env, wpath, opts, &s->session));
	}
#else
	memset(&ort->report, 0, sizeof(ort->report));
	TRY(api->CreateSession(ort->env, path, opts, &s->session));
	if (ep == OT_EP_COREML) {
		s->report = ort->report;
	}
#endif
	api->ReleaseSessionOptions(opts);
	opts = NULL;

	// I/O: one input, up to OT_ORT_MAX_OUTPUTS outputs, every dim fixed (the pack's graphs are).
	TRY(api->GetAllocatorWithDefaultOptions(&alloc));
	size_t nin = 0, nout = 0;
	TRY(api->SessionGetInputCount(s->session, &nin));
	TRY(api->SessionGetOutputCount(s->session, &nout));
	if (nin != 1 || nout == 0 || nout > OT_ORT_MAX_OUTPUTS) {
		snprintf(err, err_size, "model has %zu inputs / %zu outputs", nin, nout);
		goto fail;
	}
	{
		char *name = NULL;
		TRY(api->SessionGetInputName(s->session, 0, alloc, &name));
		s->in_name = strdup(name);
		(void)status_ok(api, api->AllocatorFree(alloc, name), NULL, 0);
		OrtTypeInfo *ti = NULL;
		TRY(api->SessionGetInputTypeInfo(s->session, 0, &ti));
		size_t rank = 0;
		s->in_shape_ok = shape_elems(api, ti, s->in_shape, 4, &rank) > 0 && rank == 4;
		api->ReleaseTypeInfo(ti);
	}
	s->out_count = (uint32_t)nout;
	for (size_t i = 0; i < nout; i++) {
		char *name = NULL;
		TRY(api->SessionGetOutputName(s->session, i, alloc, &name));
		s->out_names[i] = strdup(name);
		(void)status_ok(api, api->AllocatorFree(alloc, name), NULL, 0);
		OrtTypeInfo *ti = NULL;
		TRY(api->SessionGetOutputTypeInfo(s->session, i, &ti));
		s->out_elems[i] = shape_elems(api, ti, NULL, 0, NULL);
		api->ReleaseTypeInfo(ti);
		if (s->out_elems[i] == 0) {
			snprintf(err, err_size, "output %zu has a dynamic shape", i);
			goto fail;
		}
	}
	if (!s->in_shape_ok) {
		snprintf(err, err_size, "input is not a fixed rank-4 tensor");
		goto fail;
	}
	*out = s;
	return true;

fail:
	if (opts != NULL) {
		api->ReleaseSessionOptions(opts);
	}
	ot_session_destroy(s);
	return false;
}

void
ot_session_destroy(struct ot_session *s)
{
	if (s == NULL) {
		return;
	}
	const OrtApi *api = s->ort->api;
	if (s->session != NULL) {
		api->ReleaseSession(s->session);
	}
	if (s->cpu_mem != NULL) {
		api->ReleaseMemoryInfo(s->cpu_mem);
	}
	free(s->in_name);
	for (uint32_t i = 0; i < OT_ORT_MAX_OUTPUTS; i++) {
		free(s->out_names[i]);
	}
	free(s);
}

bool
ot_session_ep_nodes(const struct ot_session *s, uint32_t *taken, uint32_t *total, uint32_t *partitions)
{
	if (s == NULL || !s->report.valid) {
		return false;
	}
	*taken = s->report.taken;
	*total = s->report.total;
	*partitions = s->report.partitions;
	return true;
}

bool
ot_session_input_shape(const struct ot_session *s, int64_t out[4])
{
	if (s == NULL || !s->in_shape_ok) {
		return false;
	}
	memcpy(out, s->in_shape, sizeof(s->in_shape));
	return true;
}

uint32_t
ot_session_output_count(const struct ot_session *s)
{
	return s != NULL ? s->out_count : 0;
}

size_t
ot_session_output_elems(const struct ot_session *s, uint32_t i)
{
	return (s != NULL && i < s->out_count) ? s->out_elems[i] : 0;
}

bool
ot_session_run(struct ot_session *s, const float *nhwc, float *const *outs, const size_t *caps, char *err, size_t err_size)
{
	const OrtApi *api = s->ort->api;
	OrtValue *in = NULL;
	OrtValue *res[OT_ORT_MAX_OUTPUTS] = {0};
	size_t elems = 1;
	for (int i = 0; i < 4; i++) {
		elems *= (size_t)s->in_shape[i];
	}
	TRY(api->CreateTensorWithDataAsOrtValue(s->cpu_mem, (void *)nhwc, elems * sizeof(float), s->in_shape, 4,
	                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in));
	{
		const char *in_names[1] = {s->in_name};
		const OrtValue *ins[1] = {in};
		TRY(api->Run(s->session, NULL, in_names, ins, 1, (const char *const *)s->out_names, s->out_count, res));
	}
	for (uint32_t i = 0; i < s->out_count; i++) {
		if (outs[i] == NULL) {
			continue;
		}
		if (s->out_elems[i] > caps[i]) {
			snprintf(err, err_size, "output %u (%zu floats) exceeds the buffer", i, s->out_elems[i]);
			goto fail;
		}
		float *data = NULL;
		TRY(api->GetTensorMutableData(res[i], (void **)&data));
		memcpy(outs[i], data, s->out_elems[i] * sizeof(float));
	}
	for (uint32_t i = 0; i < s->out_count; i++) {
		api->ReleaseValue(res[i]);
	}
	api->ReleaseValue(in);
	return true;

fail:
	for (uint32_t i = 0; i < OT_ORT_MAX_OUTPUTS; i++) {
		if (res[i] != NULL) {
			api->ReleaseValue(res[i]);
		}
	}
	if (in != NULL) {
		api->ReleaseValue(in);
	}
	return false;
}
