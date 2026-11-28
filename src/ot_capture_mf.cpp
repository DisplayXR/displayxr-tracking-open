// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows camera capture: Media Foundation.
 *
 * What each call touches:
 *  - enumerate: MFEnumDeviceSources (video capture category) only. The
 *    IMFActivate objects are read for their friendly name and symbolic link,
 *    then released; nothing is activated, no camera opens.
 *  - authorization: the user's camera privacy switches (Settings > Privacy &
 *    security > Camera), read from the registry; nothing opens.
 *  - open: the device source + an ASYNC source reader with advanced video
 *    processing, which decodes (MJPEG) and converts to 32-bit BGRX for us. The
 *    native mode is the best one at or below the request (largest area, then
 *    the highest frame rate). Streaming starts with the first ReadSample; the
 *    reader's callback (a Media Foundation work-queue thread) copies each frame
 *    into the latest-wins mailbox and asks for the next one.
 *  - close: stop asking, flush (waits for the outstanding read), release the
 *    reader and shut the source down: the camera and its in-use indicator turn
 *    off.
 *
 * Capture time: each sample's MFSampleExtension_DeviceTimestamp (the driver's
 * QPC-based stamp, 100 ns units) when present and plausible, else the arrival
 * time. ot_monotonic_ns is QPC as well, so the two compare directly.
 *
 * Media Foundation is delay-loaded (Windows "N" editions ship without it):
 * mf_available() checks its DLLs before the first call, and without them there
 * is no camera.
 *
 * COM: every entry point initialises COM (MTA) on its calling thread for the
 * duration of the call and balances it; a thread that is already STA keeps its
 * apartment (Media Foundation's capture objects are free-threaded).
 */

#include "ot_capture_impl.h"
#include "ot_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/*!
 * MFSampleExtension_DeviceTimestamp (mfapi.h, Windows 10): the driver's QPC-based capture stamp in 100 ns
 * units. Spelled out here because not every SDK / MinGW header declares it; a sample without it reads the
 * arrival time.
 */
const GUID k_device_timestamp = {0x8f3e35e7, 0x2dcd, 0x4887, {0x86, 0x22, 0x2a, 0x58, 0xba, 0xa6, 0x52, 0xb0}};

template <class T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

//! Media Foundation's DLLs are present (they are delay-loaded: a call without them would raise).
bool
mf_available()
{
	static int s_ok = -1; // a benign race: every thread computes the same answer
	if (s_ok < 0) {
		const wchar_t *const dlls[] = {L"mfplat.dll", L"mfreadwrite.dll", L"mf.dll"};
		int ok = 1;
		for (const wchar_t *d : dlls) {
			HMODULE h = LoadLibraryExW(d, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (h == nullptr) {
				ok = 0;
				break;
			}
			FreeLibrary(h);
		}
		s_ok = ok;
	}
	return s_ok == 1;
}

//! COM for the duration of a scope, balanced.
struct com_scope
{
	bool owned = false;
	com_scope()
	{
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		owned = SUCCEEDED(hr); // S_OK or S_FALSE; RPC_E_CHANGED_MODE = an STA thread: keep it
	}
	~com_scope()
	{
		if (owned) {
			CoUninitialize();
		}
	}
	com_scope(const com_scope &) = delete;
	com_scope &
	operator=(const com_scope &) = delete;
};

//! Media Foundation for the duration of a scope (or of a capture), balanced.
struct mf_scope
{
	bool owned = false;
	mf_scope()
	{
		owned = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
	}
	~mf_scope()
	{
		if (owned) {
			MFShutdown();
		}
	}
	mf_scope(const mf_scope &) = delete;
	mf_scope &
	operator=(const mf_scope &) = delete;
};

void
wide_to_utf8(const wchar_t *w, char *out, size_t cap)
{
	if (cap == 0) {
		return;
	}
	out[0] = '\0';
	if (w == nullptr) {
		return;
	}
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr);
	if (n <= 0) {
		out[0] = '\0';
	}
	out[cap - 1] = '\0';
}

bool
utf8_to_wide(const char *s, wchar_t *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

//! The video capture devices present now (caller releases each and CoTaskMemFree's the array).
HRESULT
enum_devices(IMFActivate ***out, UINT32 *count)
{
	*out = nullptr;
	*count = 0;
	IMFAttributes *attr = nullptr;
	HRESULT hr = MFCreateAttributes(&attr, 1);
	if (SUCCEEDED(hr)) {
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = MFEnumDeviceSources(attr, out, count);
	}
	safe_release(attr);
	return hr;
}

void
free_devices(IMFActivate **devs, UINT32 n)
{
	for (UINT32 i = 0; i < n; i++) {
		safe_release(devs[i]);
	}
	CoTaskMemFree(devs);
}

//! "Deny" in one of the camera privacy switches, read without opening anything.
bool
consent_denied(HKEY root, const wchar_t *path)
{
	wchar_t v[32] = {0};
	DWORD size = sizeof(v);
	if (RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS) {
		return false;
	}
	return _wcsicmp(v, L"Deny") == 0;
}

/*
 * The reader callback: copies each frame into the mailbox and asks for the next.
 */

class reader_cb final : public IMFSourceReaderCallback
{
public:
	explicit reader_cb(struct ot_mailbox *mb) : mb_(mb)
	{
		InitializeCriticalSection(&lock_);
		flushed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	}

	bool
	ok() const
	{
		return flushed_ != nullptr;
	}

	// IUnknown
	STDMETHODIMP
	QueryInterface(REFIID iid, void **ppv) override
	{
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
			*ppv = static_cast<IMFSourceReaderCallback *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG)
	AddRef() override
	{
		return (ULONG)InterlockedIncrement(&refs_);
	}

	STDMETHODIMP_(ULONG)
	Release() override
	{
		const LONG r = InterlockedDecrement(&refs_);
		if (r == 0) {
			delete this;
		}
		return (ULONG)r;
	}

	// IMFSourceReaderCallback
	STDMETHODIMP
	OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample *sample) override
	{
		const int64_t arrival = ot_monotonic_ns();
		if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
			fail();
			return S_OK;
		}
		if (sample != nullptr) {
			deliver(sample, arrival);
		}
		// Ask for the next frame OUTSIDE the lock: the reader may call back re-entrantly.
		IMFSourceReader *next = nullptr;
		EnterCriticalSection(&lock_);
		if (!stopping_ && reader_ != nullptr) {
			next = reader_;
			next->AddRef();
		}
		LeaveCriticalSection(&lock_);
		if (next != nullptr) {
			if (FAILED(next->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr,
			                            nullptr))) {
				fail();
			}
			next->Release();
		}
		return S_OK;
	}

	STDMETHODIMP
	OnFlush(DWORD) override
	{
		SetEvent(flushed_);
		return S_OK;
	}

	STDMETHODIMP
	OnEvent(DWORD, IMFMediaEvent *) override
	{
		return S_OK;
	}

	//! The output frame size and default stride; set before the first ReadSample (no callback runs before).
	void
	configure(uint32_t w, uint32_t h, LONG default_stride)
	{
		w_ = w;
		h_ = h;
		default_stride_ = default_stride;
	}

	void
	set_reader(IMFSourceReader *r)
	{
		EnterCriticalSection(&lock_);
		reader_ = r; // raw: the reader owns us, not the reverse
		LeaveCriticalSection(&lock_);
	}

	void
	stop()
	{
		EnterCriticalSection(&lock_);
		stopping_ = true;
		LeaveCriticalSection(&lock_);
	}

	HANDLE flushed_ = nullptr;

private:
	~reader_cb()
	{
		if (flushed_ != nullptr) {
			CloseHandle(flushed_);
		}
		free(scratch_);
		DeleteCriticalSection(&lock_);
	}

	void
	fail()
	{
		bool stopping;
		EnterCriticalSection(&lock_);
		stopping = stopping_;
		LeaveCriticalSection(&lock_);
		if (!stopping) {
			ot_mailbox_set_lost(mb_); // device removed, or the stream failed
		}
	}

	int64_t
	capture_time(IMFSample *s, int64_t arrival)
	{
		UINT64 dev = 0;
		if (SUCCEEDED(s->GetUINT64(k_device_timestamp, &dev)) && dev > 0) {
			const int64_t t = (int64_t)dev * 100; // QPC in 100 ns units -> ns
			if (t <= arrival && arrival - t < 1000000000LL) {
				return t;
			}
		}
		return arrival;
	}

	void
	deliver(IMFSample *s, int64_t arrival)
	{
		IMFMediaBuffer *buf = nullptr;
		if (FAILED(s->ConvertToContiguousBuffer(&buf)) || buf == nullptr) {
			return;
		}
		const int64_t t = capture_time(s, arrival);
		BYTE *scan0 = nullptr;
		LONG pitch = 0;
		IMF2DBuffer *b2 = nullptr;
		bool locked2d = false, locked = false;
		if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
			locked2d = true;
		} else {
			BYTE *base = nullptr;
			DWORD max = 0, len = 0;
			if (SUCCEEDED(buf->Lock(&base, &max, &len))) {
				locked = true;
				pitch = default_stride_;
				const size_t row = (size_t)w_ * 4u;
				if ((size_t)len < row * h_) {
					base = nullptr;
				} else if (pitch < 0) {
					scan0 = base + (size_t)(h_ - 1) * row; // bottom-up: the top row is last
				} else {
					scan0 = base;
				}
			}
		}
		if (scan0 != nullptr) {
			if (pitch > 0) {
				ot_mailbox_put(mb_, scan0, w_, h_, (uint32_t)pitch, OT_FMT_BGRA, t, arrival);
			} else if (pitch < 0) {
				// Bottom-up RGB32: scan0 is the top row, rows go DOWN in memory by -pitch.
				const size_t row = (size_t)w_ * 4u;
				if (scratch_cap_ < row * h_) {
					void *p = realloc(scratch_, row * h_);
					if (p != nullptr) {
						scratch_ = (uint8_t *)p;
						scratch_cap_ = row * h_;
					}
				}
				if (scratch_cap_ >= row * h_) {
					for (uint32_t y = 0; y < h_; y++) {
						memcpy(scratch_ + (size_t)y * row, scan0 + (ptrdiff_t)y * (ptrdiff_t)pitch, row);
					}
					ot_mailbox_put(mb_, scratch_, w_, h_, (uint32_t)row, OT_FMT_BGRA, t, arrival);
				}
			}
		}
		if (locked2d) {
			b2->Unlock2D();
		}
		if (locked) {
			buf->Unlock();
		}
		safe_release(b2);
		safe_release(buf);
	}

	LONG refs_ = 1;
	CRITICAL_SECTION lock_;
	struct ot_mailbox *mb_;
	uint32_t w_ = 0, h_ = 0;
	LONG default_stride_ = 0;
	IMFSourceReader *reader_ = nullptr;
	bool stopping_ = false;
	uint8_t *scratch_ = nullptr; // only the callback thread touches it
	size_t scratch_cap_ = 0;
};

struct mf_cam
{
	mf_scope *mf = nullptr;
	IMFMediaSource *source = nullptr;
	IMFSourceReader *reader = nullptr;
	reader_cb *cb = nullptr;
};

void
mf_teardown(mf_cam *m)
{
	if (m->cb != nullptr) {
		m->cb->stop();
	}
	if (m->reader != nullptr) {
		// Drain the outstanding ReadSample before the reader goes away.
		if (m->cb != nullptr && SUCCEEDED(m->reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS))) {
			WaitForSingleObject(m->cb->flushed_, 2000);
		}
		if (m->cb != nullptr) {
			m->cb->set_reader(nullptr);
		}
		safe_release(m->reader);
	}
	if (m->source != nullptr) {
		m->source->Shutdown();
		safe_release(m->source);
	}
	safe_release(m->cb);
	delete m->mf;
	m->mf = nullptr;
}

void
mf_close(struct ot_capture *c)
{
	mf_cam *m = (mf_cam *)c->impl;
	if (m == nullptr) {
		return;
	}
	com_scope com;
	mf_teardown(m);
	delete m;
	c->impl = nullptr;
}

const struct ot_capture_ops g_mf_ops = {mf_close};

//! The frame rate of @p t, 0 when unknown.
double
type_fps(IMFMediaType *t)
{
	UINT32 num = 0, den = 0;
	if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) || den == 0) {
		return 0.0;
	}
	return (double)num / (double)den;
}

/*!
 * The best native mode at or below the request: the largest area, then the
 * highest frame rate (capped at the requested one). Among equals the first
 * listed wins (drivers list their preferred subtype first). AddRef'd.
 */
IMFMediaType *
pick_native(IMFSourceReader *r, const struct ot_capture_request *req, double *out_fps)
{
	IMFMediaType *best = nullptr;
	uint64_t best_area = 0;
	double best_fps = 0.0;
	for (DWORD i = 0;; i++) {
		IMFMediaType *t = nullptr;
		if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) {
			break;
		}
		UINT32 w = 0, h = 0;
		GUID major = GUID_NULL;
		const bool video = SUCCEEDED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) && major == MFMediaType_Video;
		const double fps = type_fps(t);
		if (video && SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0 &&
		    w <= req->width && h <= req->height && fps > 0.0 && fps <= (double)req->fps + 0.5) {
			const uint64_t area = (uint64_t)w * h;
			if (best == nullptr || area > best_area || (area == best_area && fps > best_fps + 0.01)) {
				safe_release(best);
				best = t;
				best->AddRef();
				best_area = area;
				best_fps = fps;
			}
		}
		safe_release(t);
	}
	*out_fps = best_fps;
	return best;
}

HRESULT
create_source(const char *link, IMFMediaSource **out)
{
	*out = nullptr;
	wchar_t wlink[1024];
	if (!utf8_to_wide(link, wlink, (int)(sizeof(wlink) / sizeof(wlink[0])))) {
		return E_INVALIDARG;
	}
	IMFAttributes *attr = nullptr;
	HRESULT hr = MFCreateAttributes(&attr, 2);
	if (SUCCEEDED(hr)) {
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wlink);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateDeviceSource(attr, out);
	}
	safe_release(attr);
	return hr;
}

void
describe_hr(HRESULT hr, char *out, size_t cap)
{
	if (hr == E_ACCESSDENIED) {
		snprintf(out, cap,
		         "access denied (Settings > Privacy & security > Camera: camera access, and \"Let desktop apps "
		         "access your camera\")");
	} else if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
	           hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) {
		snprintf(out, cap, "in use by another application (hr 0x%08lx)", (unsigned long)hr);
	} else {
		snprintf(out, cap, "hr 0x%08lx", (unsigned long)hr);
	}
}

} // namespace

extern "C" uint32_t
ot_capture_platform_enumerate(struct ot_camera_info *out, uint32_t max)
{
	if (!mf_available()) {
		return 0;
	}
	com_scope com;
	mf_scope mf;
	if (!mf.owned) {
		return 0;
	}
	IMFActivate **devs = nullptr;
	UINT32 n = 0;
	if (FAILED(enum_devices(&devs, &n))) {
		return 0;
	}
	uint32_t k = 0;
	for (UINT32 i = 0; i < n && k < max; i++) {
		WCHAR *name = nullptr, *link = nullptr;
		UINT32 len = 0;
		devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
		devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
		if (link != nullptr) {
			struct ot_camera_info *c = &out[k];
			memset(c, 0, sizeof(*c));
			wide_to_utf8(link, c->locator, sizeof(c->locator));
			wide_to_utf8(name != nullptr ? name : link, c->name, sizeof(c->name));
			// The key names the camera stably and fits OT_CAMERA_KEY_MAX: the symbolic link when it does,
			// else its tail (the device instance and the interface class; a USB path is longest at the start).
			const size_t ll = strlen(c->locator);
			ot_copy_str(c->key, sizeof(c->key),
			            ll < sizeof(c->key) ? c->locator : c->locator + (ll - (sizeof(c->key) - 1)));
			c->builtin = false; // Media Foundation does not say; the first listed is the default
			if (c->locator[0] != '\0') {
				k++;
			}
		}
		CoTaskMemFree(name);
		CoTaskMemFree(link);
	}
	free_devices(devs, n);
	return k;
}

extern "C" bool
ot_capture_platform_open(const struct ot_camera_info *cam,
                         const struct ot_capture_request *req,
                         struct ot_capture **out,
                         char *err,
                         size_t err_size)
{
	*out = nullptr;
	if (!mf_available()) {
		snprintf(err, err_size, "Media Foundation is not installed (a Windows \"N\" edition without the Media Feature Pack)");
		return false;
	}
	if (ot_capture_platform_authorization() != OT_CAMERA_AUTHORIZED) {
		snprintf(err, err_size,
		         "camera access is off (Settings > Privacy & security > Camera: camera access, and \"Let desktop "
		         "apps access your camera\")");
		return false;
	}
	com_scope com;
	mf_cam *m = new (std::nothrow) mf_cam();
	if (m == nullptr) {
		snprintf(err, err_size, "out of memory");
		return false;
	}
	m->mf = new (std::nothrow) mf_scope();
	if (m->mf == nullptr || !m->mf->owned) {
		mf_teardown(m);
		delete m;
		snprintf(err, err_size, "Media Foundation did not start");
		return false;
	}
	char why[256];
	HRESULT hr = create_source(cam->locator, &m->source);
	if (FAILED(hr)) {
		describe_hr(hr, why, sizeof(why));
		snprintf(err, err_size, "camera '%s' did not open: %s", cam->name, why);
		mf_teardown(m);
		delete m;
		return false;
	}

	struct ot_capture *c = ot_capture_alloc();
	if (c == nullptr) {
		mf_teardown(m);
		delete m;
		snprintf(err, err_size, "out of memory");
		return false;
	}
	IMFAttributes *attr = nullptr;
	IMFMediaType *native = nullptr, *outt = nullptr, *cur = nullptr;
	double fps = 0.0;
	UINT32 w = 0, h = 0;
	m->cb = new (std::nothrow) reader_cb(c->mb);
	hr = m->cb != nullptr && m->cb->ok() ? S_OK : E_OUTOFMEMORY;
	if (SUCCEEDED(hr)) {
		hr = MFCreateAttributes(&attr, 2);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback *>(m->cb));
	}
	if (SUCCEEDED(hr)) {
		// The reader inserts the decoder (MJPEG) and the colour conversion to RGB32.
		hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateSourceReaderFromMediaSource(m->source, attr, &m->reader);
	}
	if (SUCCEEDED(hr)) {
		native = pick_native(m->reader, req, &fps);
		hr = native != nullptr ? S_OK : MF_E_INVALIDMEDIATYPE;
	}
	if (SUCCEEDED(hr)) {
		hr = m->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native);
	}
	if (SUCCEEDED(hr)) {
		MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
		hr = MFCreateMediaType(&outt);
	}
	if (SUCCEEDED(hr)) {
		outt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		outt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32); // B G R X in memory: what OT_FMT_BGRA reads
		MFSetAttributeSize(outt, MF_MT_FRAME_SIZE, w, h);
		hr = m->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outt);
	}
	if (SUCCEEDED(hr)) {
		hr = m->reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
	}
	if (SUCCEEDED(hr)) {
		UINT32 cw = 0, ch = 0;
		MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &cw, &ch);
		UINT32 stride = 0;
		// MF_MT_DEFAULT_STRIDE is a signed LONG stored as UINT32: negative = bottom-up rows.
		const LONG ds = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : (LONG)cw * 4;
		if (cw == 0 || ch == 0) {
			hr = MF_E_INVALIDMEDIATYPE;
		} else {
			w = cw;
			h = ch;
			m->cb->configure(w, h, ds);
		}
	}
	if (SUCCEEDED(hr)) {
		m->cb->set_reader(m->reader);
		hr = m->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr,
		                           nullptr);
	}
	safe_release(cur);
	safe_release(outt);
	safe_release(native);
	safe_release(attr);
	if (FAILED(hr)) {
		describe_hr(hr, why, sizeof(why));
		snprintf(err, err_size, "camera '%s': no usable mode at or below %ux%u@%u (%s)", cam->name, req->width,
		         req->height, req->fps, why);
		mf_teardown(m);
		delete m;
		ot_capture_close(c);
		return false;
	}
	c->impl = m;
	c->ops = &g_mf_ops;
	c->width = w;
	c->height = h;
	c->fps = (float)fps;
	*out = c;
	return true;
}

extern "C" enum ot_camera_auth
ot_capture_platform_authorization(void)
{
	// The camera privacy switches (Windows 10 1903+): the global one (machine and user) and the one for
	// desktop (non-packaged) apps, which is what a DisplayXR process is. Missing keys = allowed.
	static const wchar_t *const k_store =
	    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
	static const wchar_t *const k_store_np =
	    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam\\NonPackaged";
	if (consent_denied(HKEY_LOCAL_MACHINE, k_store)) {
		return OT_CAMERA_RESTRICTED; // turned off for every user of this device
	}
	if (consent_denied(HKEY_CURRENT_USER, k_store) || consent_denied(HKEY_CURRENT_USER, k_store_np)) {
		return OT_CAMERA_DENIED;
	}
	return OT_CAMERA_AUTHORIZED;
}

extern "C" enum ot_camera_auth
ot_capture_platform_request_access(uint32_t timeout_ms, volatile bool *cancel)
{
	(void)timeout_ms;
	(void)cancel;
	return ot_capture_platform_authorization(); // Windows never prompts a desktop app: the switch decides
}
