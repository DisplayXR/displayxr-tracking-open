// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS camera capture through AVFoundation (ARC).
 *
 * - Enumeration (AVCaptureDeviceDiscoverySession) and the permission check
 *   (authorizationStatusForMediaType:) never open a device, so `probe` can
 *   use them. The permission prompt (requestAccessForMediaType:) is asked
 *   from the provider's thread at start. macOS attributes it to the
 *   process's responsible app: the terminal for a CLI, the app for an
 *   in-process runtime, `displayxr-tracking-host` under the service (which
 *   needs its own NSCameraUsageDescription).
 * - Mode: the device format with the largest area at or below the request,
 *   then the highest frame rate at or below the requested one. 32BGRA output
 *   (the conversion to RGB happens in the crop sampler), late frames dropped.
 * - Mirroring: forced off on the connection, so the frames are the sensor's
 *   own view (the user's right on the image's left), which is what the
 *   camera-frame maths assumes; `MIRROR` covers a source that mirrors anyway.
 *
 * Capture time. The sample buffer's presentation timestamp is on the capture
 * session's synchronization clock; it is converted to the host time clock
 * (CMClockGetHostTimeClock, i.e. mach_absolute_time ticks), then to ns
 * (mach_timebase_info). mach_absolute_time is CLOCK_UPTIME_RAW: it STOPS
 * during system sleep. The runtime's os_monotonic_get_ns() is
 * clock_gettime(CLOCK_MONOTONIC), which on macOS keeps counting through
 * sleep. The two differ by the total time slept since boot, a constant
 * between sleeps, so each frame is shifted by (CLOCK_MONOTONIC now −
 * CLOCK_UPTIME_RAW now), read at arrival. A stamp that lands in the future or
 * more than a second in the past is replaced by the arrival time (one log line).
 */

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "ot_capture_impl.h"
#include "ot_platform.h"

#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static NSArray<AVCaptureDevice *> *
video_devices(void)
{
	NSMutableArray *types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
	if (@available(macOS 14.0, *)) {
		[types addObject:AVCaptureDeviceTypeExternal];
		[types addObject:AVCaptureDeviceTypeContinuityCamera];
	} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		[types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
	}
	AVCaptureDeviceDiscoverySession *ds =
	    [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
	                                                           mediaType:AVMediaTypeVideo
	                                                            position:AVCaptureDevicePositionUnspecified];
	return ds.devices;
}

uint32_t
ot_capture_platform_enumerate(struct ot_camera_info *out, uint32_t max)
{
	@autoreleasepool {
		uint32_t n = 0;
		for (AVCaptureDevice *d in video_devices()) {
			if (n >= max) {
				break;
			}
			memset(&out[n], 0, sizeof(out[n]));
			ot_copy_str(out[n].key, sizeof(out[n].key), d.uniqueID.UTF8String);
			ot_copy_str(out[n].locator, sizeof(out[n].locator), d.uniqueID.UTF8String);
			ot_copy_str(out[n].name, sizeof(out[n].name), d.localizedName.UTF8String);
			out[n].builtin = [d.deviceType isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera];
			n++;
		}
		return n;
	}
}

static enum ot_camera_auth
map_auth(AVAuthorizationStatus s)
{
	switch (s) {
	case AVAuthorizationStatusAuthorized: return OT_CAMERA_AUTHORIZED;
	case AVAuthorizationStatusNotDetermined: return OT_CAMERA_NOT_DETERMINED;
	case AVAuthorizationStatusRestricted: return OT_CAMERA_RESTRICTED;
	default: return OT_CAMERA_DENIED;
	}
}

enum ot_camera_auth
ot_capture_platform_authorization(void)
{
	return map_auth([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]);
}

enum ot_camera_auth
ot_capture_platform_request_access(uint32_t timeout_ms, volatile bool *cancel)
{
	enum ot_camera_auth now = ot_capture_platform_authorization();
	if (now != OT_CAMERA_NOT_DETERMINED) {
		return now;
	}
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
	                         completionHandler:^(BOOL granted) {
		                         (void)granted;
		                         dispatch_semaphore_signal(sem);
	                         }];
	for (uint32_t waited = 0; waited < timeout_ms; waited += 100) {
		if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) == 0) {
			break;
		}
		if (cancel != NULL && *cancel) {
			break;
		}
	}
	return ot_capture_platform_authorization();
}

static int64_t
host_ticks_to_ns(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (int64_t)((__uint128_t)ticks * tb.numer / tb.denom);
}

@interface OTCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) struct ot_mailbox *mailbox;
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, assign) bool warned;
@end

@implementation OTCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection *)connection
{
	(void)output;
	(void)connection;
	const int64_t arrival = ot_monotonic_ns();
	// CLOCK_MONOTONIC (sleep included) minus CLOCK_UPTIME_RAW (= mach_absolute_time, sleep excluded).
	const int64_t uptime_now = (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	const int64_t sleep_offset = arrival - uptime_now;

	int64_t capture = arrival;
	CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
	if (CMTIME_IS_NUMERIC(pts)) {
		CMClockRef src = NULL;
		if (@available(macOS 12.3, *)) {
			src = self.session.synchronizationClock;
		}
		CMTime host = src != NULL ? CMSyncConvertTime(pts, src, CMClockGetHostTimeClock()) : pts;
		const int64_t t = host_ticks_to_ns(CMClockConvertHostTimeToSystemUnits(host)) + sleep_offset;
		if (t <= arrival && arrival - t < 1000000000LL) {
			capture = t;
		} else if (!self.warned) {
			self.warned = true;
			fprintf(stderr,
			        "[open-tracking] capture: frame timestamp is not on the host clock (%.1f ms from arrival); "
			        "using arrival times\n",
			        (double)(arrival - t) * 1e-6);
		}
	}

	CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sample);
	if (pb == NULL || CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA) {
		return;
	}
	if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		return;
	}
	ot_mailbox_put(self.mailbox, (const uint8_t *)CVPixelBufferGetBaseAddress(pb),
	               (uint32_t)CVPixelBufferGetWidth(pb), (uint32_t)CVPixelBufferGetHeight(pb),
	               (uint32_t)CVPixelBufferGetBytesPerRow(pb), OT_FMT_BGRA, capture, arrival);
	CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}

@end

struct avf_cam
{
	void *session;  //!< AVCaptureSession, retained (CFBridgingRetain)
	void *delegate; //!< OTCaptureDelegate, retained
	void *queue;    //!< dispatch_queue_t, retained
	void *observer; //!< disconnect notification token, retained
};

static void
avf_close(struct ot_capture *c)
{
	struct avf_cam *a = (struct avf_cam *)c->impl;
	if (a == NULL) {
		return;
	}
	@autoreleasepool {
		if (a->observer != NULL) {
			id obs = CFBridgingRelease(a->observer);
			[[NSNotificationCenter defaultCenter] removeObserver:obs];
		}
		if (a->session != NULL) {
			AVCaptureSession *s = CFBridgingRelease(a->session);
			[s stopRunning]; // synchronous: no delegate call after this returns
			for (AVCaptureOutput *o in s.outputs) {
				if ([o isKindOfClass:[AVCaptureVideoDataOutput class]]) {
					[(AVCaptureVideoDataOutput *)o setSampleBufferDelegate:nil queue:NULL];
				}
			}
		}
		if (a->queue != NULL) {
			dispatch_queue_t q = CFBridgingRelease(a->queue);
			dispatch_sync(q, ^{}); // drain a callback that was already running
		}
		if (a->delegate != NULL) {
			CFBridgingRelease(a->delegate);
		}
	}
	free(a);
	c->impl = NULL;
}

static const struct ot_capture_ops g_avf_ops = {avf_close};

//! The best format at or below the request, and its best rate at or below the requested one.
static AVCaptureDeviceFormat *
pick_format(AVCaptureDevice *dev, const struct ot_capture_request *req, double *out_fps)
{
	AVCaptureDeviceFormat *best = nil;
	int64_t best_area = -1;
	double best_rate = 0;
	for (AVCaptureDeviceFormat *f in dev.formats) {
		CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
		if ((uint32_t)d.width > req->width || (uint32_t)d.height > req->height) {
			continue;
		}
		double rate = 0;
		for (AVFrameRateRange *r in f.videoSupportedFrameRateRanges) {
			double cand = r.maxFrameRate < (double)req->fps ? r.maxFrameRate : (double)req->fps;
			if (cand >= r.minFrameRate && cand > rate) {
				rate = cand;
			}
		}
		if (rate <= 0) {
			continue;
		}
		const int64_t area = (int64_t)d.width * d.height;
		if (area > best_area || (area == best_area && rate > best_rate)) {
			best = f;
			best_area = area;
			best_rate = rate;
		}
	}
	*out_fps = best_rate;
	return best;
}

static void
apply_format(AVCaptureDevice *dev, AVCaptureDeviceFormat *fmt, double fps)
{
	NSError *e = nil;
	if (fmt == nil || ![dev lockForConfiguration:&e]) {
		return;
	}
	dev.activeFormat = fmt;
	const CMTime dur = CMTimeMake(1000, (int32_t)(fps * 1000.0 + 0.5));
	dev.activeVideoMinFrameDuration = dur;
	dev.activeVideoMaxFrameDuration = dur;
	[dev unlockForConfiguration];
}

bool
ot_capture_platform_open(const struct ot_camera_info *cam,
                         const struct ot_capture_request *req,
                         struct ot_capture **out,
                         char *err,
                         size_t err_size)
{
	*out = NULL;
	@autoreleasepool {
		AVCaptureDevice *dev = [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:cam->locator]];
		if (dev == nil) {
			snprintf(err, err_size, "camera '%s' is gone", cam->name);
			return false;
		}
		if (ot_capture_platform_authorization() != OT_CAMERA_AUTHORIZED) {
			snprintf(err, err_size, "camera permission not granted");
			return false;
		}
		NSError *e = nil;
		AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&e];
		if (input == nil) {
			snprintf(err, err_size, "camera '%s' did not open: %s", cam->name,
			         e.localizedDescription.UTF8String);
			return false;
		}
		AVCaptureSession *session = [[AVCaptureSession alloc] init];
		[session beginConfiguration];
		if (![session canAddInput:input]) {
			[session commitConfiguration];
			snprintf(err, err_size, "camera '%s': cannot add the input", cam->name);
			return false;
		}
		[session addInput:input];

		double fps = 0;
		AVCaptureDeviceFormat *fmt = pick_format(dev, req, &fps);
		apply_format(dev, fmt, fps);

		AVCaptureVideoDataOutput *vo = [[AVCaptureVideoDataOutput alloc] init];
		vo.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
		vo.alwaysDiscardsLateVideoFrames = YES;
		OTCaptureDelegate *del = [[OTCaptureDelegate alloc] init];
		struct ot_capture *c = ot_capture_alloc();
		if (c == NULL) {
			[session commitConfiguration];
			snprintf(err, err_size, "out of memory");
			return false;
		}
		del.mailbox = c->mb;
		del.session = session;
		dispatch_queue_t q = dispatch_queue_create("open-tracking.capture", DISPATCH_QUEUE_SERIAL);
		[vo setSampleBufferDelegate:del queue:q];
		if (![session canAddOutput:vo]) {
			[session commitConfiguration];
			ot_capture_close(c);
			snprintf(err, err_size, "camera '%s': cannot add the video output", cam->name);
			return false;
		}
		[session addOutput:vo];
		AVCaptureConnection *conn = [vo connectionWithMediaType:AVMediaTypeVideo];
		if (conn != nil && conn.isVideoMirroringSupported) {
			conn.automaticallyAdjustsVideoMirroring = NO;
			conn.videoMirrored = NO;
		}
		[session commitConfiguration];

		struct ot_mailbox *mb = c->mb;
		id obs = [[NSNotificationCenter defaultCenter]
		    addObserverForName:AVCaptureDeviceWasDisconnectedNotification
		                object:dev
		                 queue:nil
		            usingBlock:^(NSNotification *n) {
			            (void)n;
			            ot_mailbox_set_lost(mb);
		            }];

		[session startRunning];
		if (fmt != nil && dev.activeFormat != fmt) {
			// macOS has no input-priority preset: committing the configuration applies the session's
			// preset, which replaces the format. Setting it on the running session sticks.
			apply_format(dev, fmt, fps);
		}
		if (!session.running) {
			[[NSNotificationCenter defaultCenter] removeObserver:obs];
			ot_capture_close(c);
			snprintf(err, err_size, "camera '%s' did not start", cam->name);
			return false;
		}

		struct avf_cam *a = (struct avf_cam *)calloc(1, sizeof(*a));
		a->session = (void *)CFBridgingRetain(session);
		a->delegate = (void *)CFBridgingRetain(del);
		a->queue = (void *)CFBridgingRetain(q);
		a->observer = (void *)CFBridgingRetain(obs);
		c->impl = a;
		c->ops = &g_avf_ops;
		CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription);
		c->width = (uint32_t)d.width;
		c->height = (uint32_t)d.height;
		c->fps = (float)(dev.activeVideoMinFrameDuration.value > 0
		                     ? (double)dev.activeVideoMinFrameDuration.timescale /
		                           (double)dev.activeVideoMinFrameDuration.value
		                     : fps);
		*out = c;
		return true;
	}
}
