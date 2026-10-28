// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common defines and enums for XRT.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Nis Madsen <nima_zero_one@protonmail.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

#include <assert.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Internal define for VK_UUID_SIZE and XR_UUID_SIZE_EXT.
 *
 * @ingroup xrt_iface
 */
#define XRT_UUID_SIZE 16

/*!
 * To transport UUIDs between different APIs.
 *
 * @ingroup xrt_iface
 */
struct xrt_uuid
{
	uint8_t data[XRT_UUID_SIZE];
};

/*!
 * Typedef for @ref xrt_uuid.
 *
 * @ingroup xrt_iface
 */
typedef struct xrt_uuid xrt_uuid_t;

/*!
 * Internal define for VK_LUID_SIZE.
 *
 * @ingroup xrt_iface
 */
#define XRT_LUID_SIZE 8

/*!
 * To transport LUIDs between different APIs.
 *
 * @ingroup xrt_iface
 */
struct xrt_luid
{
	uint8_t data[XRT_LUID_SIZE];
};

/*!
 * Typedef for @ref xrt_luid.
 *
 * @ingroup xrt_iface
 */
typedef struct xrt_luid xrt_luid_t;

/*!
 * A limited unique id, it is only unique for the process it is in, so must not be
 * used or synchronized across process boundaries. A value of zero is invalid
 * and means it has not be properly initialised.
 *
 * @ingroup xrt_iface
 */
struct xrt_limited_unique_id
{
	uint64_t data;
};

/*!
 * Typedef for @ref xrt_limited_unique_id.
 *
 * @ingroup xrt_iface
 */
typedef struct xrt_limited_unique_id xrt_limited_unique_id_t;

/*!
 * A base class for reference counted objects.
 *
 * @ingroup xrt_iface
 */
struct xrt_reference
{
	xrt_atomic_s32_t count;
};

/*!
 * Blend mode that the device supports, exact mirror of XrEnvironmentBlendMode.
 *
 * This is not a bitmask because we want to be able to express a preference order
 * that may vary by device, etc.
 *
 * @ingroup xrt_iface
 */
enum xrt_blend_mode
{
	XRT_BLEND_MODE_OPAQUE = 1,
	XRT_BLEND_MODE_ADDITIVE = 2,
	XRT_BLEND_MODE_ALPHA_BLEND = 3,
	XRT_BLEND_MODE_MAX_ENUM,
};

#define XRT_MAX_DEVICE_BLEND_MODES 3

/*!
 * Special flags for creating passthrough.
 */
enum xrt_passthrough_create_flags
{
	//! Start the passthrough on creation
	XRT_PASSTHROUGH_IS_RUNNING_AT_CREATION = (1 << 0),
	//! Our compositor just ignores this bit.
	XRT_PASSTHROUGH_LAYER_DEPTH = (1 << 1),
};

/*!
 * Specify additional state change behavior.
 */
enum xrt_passthrough_state
{
	//! Passthrough system requires reinitialization.
	XRT_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT = (1 << 0),
	//! Non-recoverable error has occurred.
	XRT_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT = (1 << 1),
	//! A recoverable error has occurred.
	XRT_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT = (1 << 2),
	//! The runtime has recovered from a previous error and is functioning normally.
	XRT_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT = (1 << 3),
};

/*!
 * Specify the kind of passthrough behavior the layer provides.
 */
enum xrt_passthrough_purpose_flags
{
	//! Fullscreen layer
	XRT_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION = (1 << 0),
	//! Projected layer.
	XRT_PASSTHROUGH_LAYER_PURPOSE_PROJECTED = (1 << 1),
	XRT_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_HANDS = 1000203001,
	XRT_PASSTHROUGH_LAYER_PURPOSE_TRACKED_KEYBOARD_MASKED_HANDS = 1000203002,
};

/*!
 * Which distortion model does the device expose,
 * used both as a bitfield and value.
 */
enum xrt_distortion_model
{
	// clang-format off
	XRT_DISTORTION_MODEL_NONE      = 1u << 0u,
	XRT_DISTORTION_MODEL_COMPUTE   = 1u << 1u,
	XRT_DISTORTION_MODEL_MESHUV    = 1u << 2u,
	// clang-format on
};

/*!
 * Common formats, use `u_format_*` functions to reason about them.
 */
enum xrt_format
{
	XRT_FORMAT_R8G8B8X8,
	XRT_FORMAT_R8G8B8A8,
	XRT_FORMAT_R8G8B8,
	XRT_FORMAT_R8G8,
	XRT_FORMAT_R8,

	XRT_FORMAT_BAYER_GR8,

	XRT_FORMAT_L8, // Luminence, R = L, G = L, B = L.

	XRT_FORMAT_BITMAP_8X1, // One bit format tiled in 8x1 blocks.
	XRT_FORMAT_BITMAP_8X8, // One bit format tiled in 8X8 blocks.

	XRT_FORMAT_YUV888,
	XRT_FORMAT_YUYV422,
	XRT_FORMAT_UYVY422,

	XRT_FORMAT_MJPEG,

	XRT_FORMAT_BC4,
};

/*!
 * What type of stereo format a frame has.
 *
 * @ingroup xrt_iface
 */
enum xrt_stereo_format
{
	XRT_STEREO_FORMAT_NONE,
	XRT_STEREO_FORMAT_SBS,         //!< Side by side.
	XRT_STEREO_FORMAT_INTERLEAVED, //!< Interleaved pixels.
	XRT_STEREO_FORMAT_OAU,         //!< Over & Under.
};

/*!
 * A quaternion with single floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_quat
{
	float x;
	float y;
	float z;
	float w;
};

/*!
 * Identity value for @ref xrt_quat
 *
 * @ingroup xrt_iface math
 * @relates xrt_quat
 */
// clang-format off
#define XRT_QUAT_IDENTITY {0.f, 0.f, 0.f, 1.f}
// clang-format on

/*!
 * A 1 element vector with single floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec1
{
	float x;
};

/*!
 * A 2 element vector with single floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec2
{
	float x;
	float y;
};

/*!
 * Represents a uv triplet for distortion, basically just three xrt_vec2.
 *
 * @ingroup xrt_iface_math
 */
struct xrt_uv_triplet
{
	struct xrt_vec2 r, g, b;
};

/*!
 * A 3 element vector with single floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec3
{
	float x;
	float y;
	float z;
};

/*!
 * A 3 element vector with single doubles.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec3_f64
{
	double x;
	double y;
	double z;
};

/*!
 * All-zero value for @ref xrt_vec3
 *
 * @ingroup xrt_iface math
 * @relates xrt_vec3
 */
// clang-format off
#define XRT_VEC3_ZERO {0.f, 0.f, 0.f}
// clang-format on
/*!
 * Value for @ref xrt_vec3 with 1 in the @p x coordinate.
 *
 * @ingroup xrt_iface math
 * @relates xrt_vec3
 */
// clang-format off
#define XRT_VEC3_UNIT_X {1.f, 0.f, 0.f}
// clang-format on
/*!
 * Value for @ref xrt_vec3 with 1 in the @p y coordinate.
 *
 * @ingroup xrt_iface math
 * @relates xrt_vec3
 */
// clang-format off
#define XRT_VEC3_UNIT_Y {0.f, 1.f, 0.f}
// clang-format on
/*!
 * Value for @ref xrt_vec3 with 1 in the @p z coordinate.
 *
 * @ingroup xrt_iface math
 * @relates xrt_vec3
 */
// clang-format off
#define XRT_VEC3_UNIT_Z {0.f, 0.f, 1.f}
// clang-format on

/*!
 * A 3 element vector with 32 bit integers.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec3_i32
{
	int32_t x;
	int32_t y;
	int32_t z;
};

/*!
 * A 2 element vector with 32 bit integers.
 *
 * @ingroup xrt_iface math
 */
struct xrt_vec2_i32
{
	int32_t x;
	int32_t y;
};

/*!
 * A 3 element colour with 8 bits per channel.
 *
 * @ingroup xrt_iface math
 */
struct xrt_colour_rgb_u8
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/*!
 * A 4 element colour with 8 bits per channel.
 *
 * @ingroup xrt_iface math
 */
struct xrt_colour_rgba_u8
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

/*!
 * A 3 element colour with floating point channels.
 *
 * @ingroup xrt_iface math
 */
struct xrt_colour_rgb_f32
{
	float r;
	float g;
	float b;
};

/*!
 * A 4 element colour with floating point channels.
 *
 * @ingroup xrt_iface math
 */
struct xrt_colour_rgba_f32
{
	float r;
	float g;
	float b;
	float a;
};

/*!
 * Image size.
 *
 * @ingroup xrt_iface math
 */
struct xrt_size
{
	int w;
	int h;
};

/*!
 * Image offset.
 *
 * @ingroup xrt_iface math
 */
struct xrt_offset
{
	int w, h;
};

/*!
 * Image rectangle.
 *
 * @ingroup xrt_iface math
 */
struct xrt_rect
{
	struct xrt_offset offset;
	struct xrt_size extent;
};

/*!
 * Image rectangle
 *
 * @todo Unify xrt_rect and xrt_rect_f32 field names
 *
 * @ingroup xrt_iface math
 */
struct xrt_rect_f32
{
	float x, y, w, h;
};

/*!
 * Normalized image rectangle, coordinates and size in 0 .. 1 range.
 *
 * @ingroup xrt_iface math
 */
struct xrt_normalized_rect
{
	float x, y, w, h;
};

/*!
 * A pose composed of a position and orientation.
 *
 * @see xrt_qaut
 * @see xrt_vec3
 * @ingroup xrt_iface math
 */
struct xrt_pose
{
	struct xrt_quat orientation;
	struct xrt_vec3 position;
};
/*!
 * Identity value for @ref xrt_pose
 *
 * @ingroup xrt_iface math
 * @relates xrt_pose
 */
// clang-format off
#define XRT_POSE_IDENTITY {XRT_QUAT_IDENTITY, XRT_VEC3_ZERO}
// clang-format on

/*!
 * Describes a projection matrix fov.
 *
 * @ingroup xrt_iface math
 */
struct xrt_fov
{
	float angle_left;
	float angle_right;
	float angle_up;
	float angle_down;
};

/*!
 * The number of values in @ref xrt_matrix_2x2
 *
 * @ingroup xrt_iface math
 */
#define XRT_MATRIX_2X2_ELEMENTS 4

/*!
 * The number of 2d vectors in @ref xrt_matrix_2x2
 *
 * @ingroup xrt_iface math
 */
#define XRT_MATRIX_2X2_VECS 2

/*!
 * A tightly packed 2x2 matrix of floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_matrix_2x2
{
	union {
		float v[XRT_MATRIX_2X2_ELEMENTS];
		struct xrt_vec2 vecs[XRT_MATRIX_2X2_VECS];
	};
};

/*!
 * The number of values in @ref xrt_matrix_3x3
 *
 * @ingroup xrt_iface math
 */
#define XRT_MATRIX_3X3_ELEMENTS 9

/*!
 * A tightly packed 3x3 matrix of floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_matrix_3x3
{
	float v[XRT_MATRIX_3X3_ELEMENTS];
};

/*!
 * A tightly packed 3x3 matrix of doubles.
 *
 * @ingroup xrt_iface math
 */
struct xrt_matrix_3x3_f64
{
	double v[XRT_MATRIX_3X3_ELEMENTS];
};

/*!
 * The number of values in a 4x4 matrix like @ref xrt_matrix_4x4 and @ref xrt_matrix_4x4_f64
 *
 * @ingroup xrt_iface math
 */
#define XRT_MATRIX_4X4_ELEMENTS 16

/*!
 * A tightly packed 4x4 matrix of floats.
 *
 * @ingroup xrt_iface math
 */
struct xrt_matrix_4x4
{
	float v[XRT_MATRIX_4X4_ELEMENTS];
};

/*!
 * A tightly packed 4x4 matrix of double.
 *
 * @ingroup xrt_iface math
 */
struct xrt_matrix_4x4_f64
{
	double v[XRT_MATRIX_4X4_ELEMENTS];
};

/*!
 * A range of API versions supported.
 *
 * @ingroup xrt_iface math
 */
struct xrt_api_requirements
{
	uint32_t min_major;
	uint32_t min_minor;
	uint32_t min_patch;

	uint32_t max_major;
	uint32_t max_minor;
	uint32_t max_patch;
};

/*!
 * Type of a OpenXR mapped reference space, maps to the semantic spaces on the
 * @ref xrt_space_overseer struct. This is used to refer to indirectly for
 * instance when letting the overseer know that an application is using a
 * particular reference space.
 *
 * @ingroup xrt_iface
 */
enum xrt_reference_space_type
{
	XRT_SPACE_REFERENCE_TYPE_VIEW,
	XRT_SPACE_REFERENCE_TYPE_LOCAL,
	XRT_SPACE_REFERENCE_TYPE_LOCAL_FLOOR,
	XRT_SPACE_REFERENCE_TYPE_STAGE,
	XRT_SPACE_REFERENCE_TYPE_UNBOUNDED,
};

/*!
 * The number of enumerations in @ref xrt_reference_space_type.
 *
 * @ingroup xrt_iface
 */
#define XRT_SPACE_REFERENCE_TYPE_COUNT (XRT_SPACE_REFERENCE_TYPE_UNBOUNDED + 1)

/*!
 * An invalid @ref xrt_reference_space_type, since it's invalid it's not listed
 * in the enum.
 *
 * @ingroup xrt_iface
 */
#define XRT_SPACE_REFERENCE_TYPE_INVALID ((enum xrt_reference_space_type)(-1))

/*!
 * Flags of which components of a @ref xrt_space_relation is valid.
 *
 * @see xrt_space_relation
 * @ingroup xrt_iface math
 */
enum xrt_space_relation_flags
{
	// clang-format off
	XRT_SPACE_RELATION_ORIENTATION_VALID_BIT =          (1u << 0u),
	XRT_SPACE_RELATION_POSITION_VALID_BIT =             (1u << 1u),
	XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT =      (1u << 2u),
	XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT =     (1u << 3u),
	XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT =        (1u << 4u),
	XRT_SPACE_RELATION_POSITION_TRACKED_BIT =           (1u << 5u),
	// clang-format on
	XRT_SPACE_RELATION_BITMASK_ALL = (uint32_t)XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |      //
	                                 (uint32_t)XRT_SPACE_RELATION_POSITION_VALID_BIT |         //
	                                 (uint32_t)XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |  //
	                                 (uint32_t)XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT | //
	                                 (uint32_t)XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |    //
	                                 (uint32_t)XRT_SPACE_RELATION_POSITION_TRACKED_BIT,
	XRT_SPACE_RELATION_BITMASK_NONE = 0,
};

/*!
 * A relation with two spaces, includes velocity and acceleration.
 *
 * @see xrt_quat
 * @see xrt_vec3
 * @see xrt_pose
 * @see xrt_space_relation_flags
 * @ingroup xrt_iface math
 */
struct xrt_space_relation
{
	enum xrt_space_relation_flags relation_flags;
	struct xrt_pose pose;
	struct xrt_vec3 linear_velocity;
	struct xrt_vec3 angular_velocity;
};

/*!
 * A zero/identity value for @ref xrt_space_relation
 *
 * @note Despite this initializing all members (to zero or identity), this sets the xrt_space_relation::relation_flags
 * to XRT_SPACE_RELATION_BITMASK_NONE - so this is safe to assign before an error return, etc.
 *
 * @ingroup xrt_iface math
 * @relates xrt_space_relation
 */
// clang-format off
#define XRT_SPACE_RELATION_ZERO {XRT_SPACE_RELATION_BITMASK_NONE, XRT_POSE_IDENTITY, XRT_VEC3_ZERO, XRT_VEC3_ZERO}
// clang-format on

/*!
 * The maximum number of steps that can be in a relation chain.
 *
 * @see xrt_relation_chain::steps
 * @relates xrt_relation_chain
 * @ingroup xrt_iface math
 */
#define XRT_RELATION_CHAIN_CAPACITY 8

/*!
 * A chain of space relations and their associated validity flags.
 * Functions for manipulating this are available in `math/m_space.h`.
 *
 * @see xrt_space_relation
 * @ingroup xrt_iface math
 */
struct xrt_relation_chain
{
	struct xrt_space_relation steps[XRT_RELATION_CHAIN_CAPACITY];
	uint32_t step_count;
};


/*
 *
 * Input related enums and structs.
 *
 */

/*!
 * A enum that is used to name devices so that the
 * state trackers can reason about the devices easier.
 */
enum xrt_device_name
{
	XRT_DEVICE_INVALID = 0,

	XRT_DEVICE_GENERIC_HMD = 1,


	// "Controllers" somewhat sorted as listed in spec.
	XRT_DEVICE_SIMPLE_CONTROLLER = 9,


	XRT_DEVICE_HAND_INTERACTION = 20,

	XRT_DEVICE_EYE_GAZE_INTERACTION = 21,


	// Other misc stuff.
	XRT_DEVICE_HAND_TRACKER = 27,

	//! XR_EXT_hand_interaction
	XRT_DEVICE_EXT_HAND_INTERACTION = 30,




	// added in OpenXR 1.1


};

/*!
 * How an xrt_device can be used.
 *
 * @ingroup xrt_iface
 */
enum xrt_device_type
{
	XRT_DEVICE_TYPE_UNKNOWN = 0,
	XRT_DEVICE_TYPE_HMD,
	XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER,
	XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER,
	XRT_DEVICE_TYPE_ANY_HAND_CONTROLLER,
	XRT_DEVICE_TYPE_GAMEPAD,
	XRT_DEVICE_TYPE_GENERIC_TRACKER,
	XRT_DEVICE_TYPE_HAND_TRACKER,
	XRT_DEVICE_TYPE_EYE_TRACKER,
	XRT_DEVICE_TYPE_FACE_TRACKER,
	XRT_DEVICE_TYPE_BODY_TRACKER,
	//! Holds the rig (navigation) role, ADR-034 Amendment 4.
	XRT_DEVICE_TYPE_NAVIGATION,
};

/*!
 * Base type of this inputs.
 *
 * @ingroup xrt_iface
 */
enum xrt_input_type
{
	// clang-format off
	//! Float input in [0, 1]
	XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE      = 0x00,
	//! Float input in [-1, 1]
	XRT_INPUT_TYPE_VEC1_MINUS_ONE_TO_ONE = 0x01,
	//! Vec2 input, components in [-1, 1]
	XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE = 0x02,
	//! Vec3 input, components in [-1, 1]
	XRT_INPUT_TYPE_VEC3_MINUS_ONE_TO_ONE = 0x03,
	//! Boolean (digital, binary) input
	XRT_INPUT_TYPE_BOOLEAN               = 0x04,
	//! A tracked pose
	XRT_INPUT_TYPE_POSE                  = 0x05,
	//! A tracked hand
	XRT_INPUT_TYPE_HAND_TRACKING         = 0x06,
	//! A tracked face
	XRT_INPUT_TYPE_FACE_TRACKING         = 0x07,
	//! A tracked body
	XRT_INPUT_TYPE_BODY_TRACKING         = 0x08,
	// clang-format on
};

/*!
 * The number of bits reserved for the input type in @ref xrt_input_name
 *
 * @see xrt_input_name
 * @ingroup xrt_iface
 */
#define XRT_INPUT_TYPE_BITWIDTH 8u

/*!
 * The mask associated with @ref XRT_INPUT_TYPE_BITWIDTH
 *
 * @see xrt_input_name
 * @ingroup xrt_iface
 */

#define XRT_INPUT_TYPE_BITMASK 0xffu

/*!
 * @brief Create an enum value for xrt_input_name that packs an ID and input
 * type.
 *
 * @param id an integer
 * @param type The suffix of an xrt_input_type value name: `XRT_INPUT_TYPE_` is
 * prepended automatically.
 *
 * @see xrt_input_name
 * @ingroup xrt_iface
 */
#define XRT_INPUT_NAME(id, type) ((UINT32_C(id) << XRT_INPUT_TYPE_BITWIDTH) | (uint32_t)XRT_INPUT_TYPE_##type)

/*!
 * @brief Extract the xrt_input_type from an xrt_input_name.
 *
 * @param name An xrt_input_name value
 *
 * @relates xrt_input_name
 * @returns @ref xrt_input_type
 * @ingroup xrt_iface
 */
#define XRT_GET_INPUT_TYPE(name) ((enum xrt_input_type)(name & XRT_INPUT_TYPE_BITMASK))

/*!
 * @brief Extract the xrt_input_name id from an xrt_input_name.
 *
 * @param name An xrt_input_name value
 *
 * @relates xrt_input_name
 * @returns @ref xrt_input_type
 * @ingroup xrt_iface
 */
#define XRT_GET_INPUT_ID(name) ((uint32_t)(name >> XRT_INPUT_TYPE_BITWIDTH))

// clang-format off
#define XRT_INPUT_LIST(_) \
	/** Standard pose used for rendering */  \
	_(XRT_INPUT_GENERIC_HEAD_POSE                      , XRT_INPUT_NAME(0x0000, POSE)) \
	_(XRT_INPUT_GENERIC_HEAD_DETECT                    , XRT_INPUT_NAME(0x0001, BOOLEAN)) \
	_(XRT_INPUT_HT_UNOBSTRUCTED_LEFT                   , XRT_INPUT_NAME(0x0002, HAND_TRACKING)) \
	_(XRT_INPUT_HT_UNOBSTRUCTED_RIGHT                  , XRT_INPUT_NAME(0x0003, HAND_TRACKING)) \
	_(XRT_INPUT_HT_CONFORMING_LEFT                     , XRT_INPUT_NAME(0x0004, HAND_TRACKING)) \
	_(XRT_INPUT_HT_CONFORMING_RIGHT                    , XRT_INPUT_NAME(0x0005, HAND_TRACKING)) \
	_(XRT_INPUT_GENERIC_TRACKER_POSE                   , XRT_INPUT_NAME(0x0006, POSE)) \
	/** XR_EXT_palm_pose */ \
	_(XRT_INPUT_GENERIC_PALM_POSE                      , XRT_INPUT_NAME(0x0007, POSE)) \
\
	/** XR_EXT_eye_gaze_interaction */ \
	_(XRT_INPUT_GENERIC_EYE_GAZE_POSE                  , XRT_INPUT_NAME(0x0008, POSE)) \
	/** Standard non-view reference spaces */ \
	_(XRT_INPUT_GENERIC_LOCAL_SPACE_POSE               , XRT_INPUT_NAME(0x0009, POSE)) \
	_(XRT_INPUT_GENERIC_LOCAL_FLOOR_SPACE_POSE         , XRT_INPUT_NAME(0x000A, POSE)) \
	_(XRT_INPUT_GENERIC_STAGE_SPACE_POSE               , XRT_INPUT_NAME(0x000B, POSE)) \
	_(XRT_INPUT_GENERIC_UNBOUNDED_SPACE_POSE           , XRT_INPUT_NAME(0x000C, POSE)) \
	/** Rig (navigation) role, ADR-034 Amendment 4 */ \
	_(XRT_INPUT_GENERIC_NAVIGATION_POSE                , XRT_INPUT_NAME(0x000D, POSE)) \
	_(XRT_INPUT_GENERIC_NAVIGATION_RECENTER            , XRT_INPUT_NAME(0x000E, BOOLEAN)) \
\
	_(XRT_INPUT_SIMPLE_SELECT_CLICK                    , XRT_INPUT_NAME(0x0010, BOOLEAN)) \
	_(XRT_INPUT_SIMPLE_MENU_CLICK                      , XRT_INPUT_NAME(0x0011, BOOLEAN)) \
	_(XRT_INPUT_SIMPLE_GRIP_POSE                       , XRT_INPUT_NAME(0x0012, POSE)) \
	_(XRT_INPUT_SIMPLE_AIM_POSE                        , XRT_INPUT_NAME(0x0013, POSE)) \
\
	_(XRT_INPUT_HAND_SELECT_VALUE                      , XRT_INPUT_NAME(0x00E0, VEC1_ZERO_TO_ONE)) \
	_(XRT_INPUT_HAND_SQUEEZE_VALUE                     , XRT_INPUT_NAME(0x00E1, VEC1_ZERO_TO_ONE)) \
	_(XRT_INPUT_HAND_GRIP_POSE                         , XRT_INPUT_NAME(0x00E2, POSE)) \
	_(XRT_INPUT_HAND_AIM_POSE                          , XRT_INPUT_NAME(0x00E3, POSE)) \
\
	/** XR_EXT_hand_interaction */ \
	_(XRT_INPUT_HAND_PINCH_POSE                        , XRT_INPUT_NAME(0x0401, POSE)) \
	_(XRT_INPUT_HAND_POKE_POSE                         , XRT_INPUT_NAME(0x0402, POSE)) \
	_(XRT_INPUT_HAND_PINCH_VALUE                       , XRT_INPUT_NAME(0x0403, VEC1_ZERO_TO_ONE)) \
	_(XRT_INPUT_HAND_AIM_ACTIVATE_VALUE                , XRT_INPUT_NAME(0x0404, VEC1_ZERO_TO_ONE)) \
	_(XRT_INPUT_HAND_GRASP_VALUE                       , XRT_INPUT_NAME(0x0405, VEC1_ZERO_TO_ONE)) \
	_(XRT_INPUT_HAND_PINCH_READY                       , XRT_INPUT_NAME(0x0406, BOOLEAN)) \
	_(XRT_INPUT_HAND_AIM_ACTIVATE_READY                , XRT_INPUT_NAME(0x0407, BOOLEAN)) \
	_(XRT_INPUT_HAND_GRASP_READY                       , XRT_INPUT_NAME(0x0408, BOOLEAN)) \
\
	_(XRT_INPUT_GENERIC_FACE_TRACKING                  , XRT_INPUT_NAME(0x0600, FACE_TRACKING)) \
\
	_(XRT_INPUT_GENERIC_BODY_TRACKING                  , XRT_INPUT_NAME(0x0700, BODY_TRACKING)) \
\

// clang-format on


/*!
 * Every internal input source known to monado with a baked in type.
 * Values are maintained in XRT_INPUT_LIST.
 *
 * @see xrt_input_type
 * @ingroup xrt_iface
 */
enum xrt_input_name
{
#define XRT_INPUT_LIST_TO_NAME_VALUE(NAME, VALUE) NAME = VALUE,

	XRT_INPUT_LIST(XRT_INPUT_LIST_TO_NAME_VALUE)

#undef XRT_INPUT_LIST_TO_NAME_VALUE
};

/*!
 * Number of joints in a hand. Corresponds to XR_HAND_JOINT_COUNT_EXT.
 *
 * @see xrt_hand_joint
 * @ingroup xrt_iface
 */
#define XRT_HAND_JOINT_COUNT 26

/*!
 * Number of joints in a hand. Corresponds to XrHandJointEXT.
 *
 * @ingroup xrt_iface
 */
enum xrt_hand_joint
{
	XRT_HAND_JOINT_PALM = 0,
	XRT_HAND_JOINT_WRIST = 1,
	XRT_HAND_JOINT_THUMB_METACARPAL = 2,
	XRT_HAND_JOINT_THUMB_PROXIMAL = 3,
	XRT_HAND_JOINT_THUMB_DISTAL = 4,
	XRT_HAND_JOINT_THUMB_TIP = 5,
	XRT_HAND_JOINT_INDEX_METACARPAL = 6,
	XRT_HAND_JOINT_INDEX_PROXIMAL = 7,
	XRT_HAND_JOINT_INDEX_INTERMEDIATE = 8,
	XRT_HAND_JOINT_INDEX_DISTAL = 9,
	XRT_HAND_JOINT_INDEX_TIP = 10,
	XRT_HAND_JOINT_MIDDLE_METACARPAL = 11,
	XRT_HAND_JOINT_MIDDLE_PROXIMAL = 12,
	XRT_HAND_JOINT_MIDDLE_INTERMEDIATE = 13,
	XRT_HAND_JOINT_MIDDLE_DISTAL = 14,
	XRT_HAND_JOINT_MIDDLE_TIP = 15,
	XRT_HAND_JOINT_RING_METACARPAL = 16,
	XRT_HAND_JOINT_RING_PROXIMAL = 17,
	XRT_HAND_JOINT_RING_INTERMEDIATE = 18,
	XRT_HAND_JOINT_RING_DISTAL = 19,
	XRT_HAND_JOINT_RING_TIP = 20,
	XRT_HAND_JOINT_LITTLE_METACARPAL = 21,
	XRT_HAND_JOINT_LITTLE_PROXIMAL = 22,
	XRT_HAND_JOINT_LITTLE_INTERMEDIATE = 23,
	XRT_HAND_JOINT_LITTLE_DISTAL = 24,
	XRT_HAND_JOINT_LITTLE_TIP = 25,
	XRT_HAND_JOINT_MAX_ENUM = 0x7FFFFFFF
};

/*!
 * Enumeration for left and right hand.
 *
 * @ingroup xrt_iface
 */
enum xrt_hand
{
	XRT_HAND_LEFT = 0,
	XRT_HAND_RIGHT = 1,
};

/*!
 * Location of a single hand joint. Corresponds to XrHandJointLocationEXT.
 *
 * @ingroup xrt_iface
 */
struct xrt_hand_joint_value
{
	struct xrt_space_relation relation;
	float radius;
};

/*!
 * Number of fingers on a hand.
 *
 * @ingroup xrt_iface
 */
#define XRT_FINGER_COUNT 5

/*!
 * Names for fingers on a hand.
 *
 * @ingroup xrt_iface
 */
enum xrt_finger
{
	XRT_FINGER_LITTLE = 0,
	XRT_FINGER_RING,
	XRT_FINGER_MIDDLE,
	XRT_FINGER_INDEX,
	XRT_FINGER_THUMB
};

/*!
 * Joint set type used for hand tracking. Corresponds to XrHandJointSetEXT.
 *
 * @ingroup xrt_iface
 */
struct xrt_hand_joint_set
{
	union {
		struct xrt_hand_joint_value hand_joint_set_default[XRT_HAND_JOINT_COUNT];
	} values;

	// in driver global space, without tracking_origin offset
	struct xrt_space_relation hand_pose;
	bool is_active;
};

/*!
 * A union of all input types.
 *
 * @see xrt_input_type
 * @ingroup xrt_iface math
 */
union xrt_input_value {
	struct xrt_vec1 vec1;
	struct xrt_vec2 vec2;
	bool boolean;
};

/*!
 * The number of bits reserved for the input type in @ref xrt_output_name
 * @see xrt_output_type
 * @ingroup xrt_iface
 */
#define XRT_OUTPUT_TYPE_BITWIDTH 8u

/*!
 * The mask associated with @ref XRT_OUTPUT_TYPE_BITWIDTH
 * @see xrt_output_type
 * @ingroup xrt_iface
 */
#define XRT_OUTPUT_TYPE_BITMASK 0xffu

/*!
 * Base type of this output.
 *
 * @ingroup xrt_iface
 */
enum xrt_output_type
{
	// clang-format off
	XRT_OUTPUT_TYPE_VIBRATION             = 0x00,
	XRT_OUTPUT_TYPE_FORCE_FEEDBACK        = 0x01,
	// clang-format on
};

#define XRT_OUTPUT_NAME(id, type) ((UINT32_C(id) << XRT_OUTPUT_TYPE_BITWIDTH) | (uint32_t)XRT_OUTPUT_TYPE_##type)

/*
 * Face and body tracking. The provider never fills these; struct xrt_device
 * passes them by pointer, so incomplete types are enough here.
 */
struct xrt_facial_expression_set;
struct xrt_body_skeleton;
struct xrt_body_joint_set;

/*!
 * Name of a output with a baked in type.
 *
 * @see xrt_output_type
 * @ingroup xrt_iface
 */
enum xrt_output_name
{
	// clang-format off
	XRT_OUTPUT_NAME_SIMPLE_VIBRATION            = XRT_OUTPUT_NAME(0x0010, VIBRATION),

	XRT_OUTPUT_NAME_FORCE_FEEDBACK_LEFT         = XRT_OUTPUT_NAME(0x0080, FORCE_FEEDBACK),
	XRT_OUTPUT_NAME_FORCE_FEEDBACK_RIGHT        = XRT_OUTPUT_NAME(0x0081, FORCE_FEEDBACK),

	// clang-format on
};

/*!
 * Value used to indicate a haptic pulse of the minimal supported duration.
 *
 * @ingroup xrt_iface
 */
#define XRT_MIN_HAPTIC_DURATION -1

/*!
 * Value used to indicate a haptic pulse of some runtime defined optimal
 * frequency.
 *
 * @ingroup xrt_iface
 */

#define XRT_FREQUENCY_UNSPECIFIED 0

/*!
 * Value used as a timeout to indicate the timeout should never occur.
 *
 * @ingroup xrt_iface
 */
#define XRT_INFINITE_DURATION (0x7fffffffffffffffLL)

enum xrt_force_feedback_location
{
	XRT_FORCE_FEEDBACK_LOCATION_LEFT_THUMB,
	XRT_FORCE_FEEDBACK_LOCATION_LEFT_INDEX,
	XRT_FORCE_FEEDBACK_LOCATION_LEFT_MIDDLE,
	XRT_FORCE_FEEDBACK_LOCATION_LEFT_RING,
	XRT_FORCE_FEEDBACK_LOCATION_LEFT_PINKY,
};

struct xrt_output_force_feedback
{
	float value;
	enum xrt_force_feedback_location location;
};

enum xrt_output_value_type
{
	XRT_OUTPUT_VALUE_TYPE_VIBRATION,
	XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION,
	XRT_OUTPUT_VALUE_TYPE_FORCE_FEEDBACK,
};

struct xrt_output_value_vibration
{
	XRT_ALIGNAS(8) float frequency;
	float amplitude;
	int64_t duration_ns;
};

struct xrt_output_value_pcm_vibration
{
	uint32_t buffer_size;
	XRT_ALIGNAS(8) const float *buffer;
	XRT_ALIGNAS(8) float sample_rate;
	bool append;
	uint32_t *samples_consumed;
};

struct xrt_output_value_force_feedback
{
	struct xrt_output_force_feedback force_feedback[5];
	uint64_t force_feedback_location_count;
};

/*!
 * A union of all output types.
 *
 * @see xrt_output_type
 * @ingroup xrt_iface math
 */
struct xrt_output_value
{
	enum xrt_output_value_type type;

	union {
		struct xrt_output_value_vibration vibration;
		struct xrt_output_value_pcm_vibration pcm_vibration;
		struct xrt_output_value_force_feedback force_feedback;
	};
};

/*
 *
 * Misc enums.
 *
 */

/*!
 * What form factor is this device, mostly maps onto OpenXR's @p XrFormFactor.
 *
 * @ingroup xrt_iface
 */
enum xrt_form_factor
{
	XRT_FORM_FACTOR_HMD,      //!< Head mounted display.
	XRT_FORM_FACTOR_HANDHELD, //!< Handheld display.
};

/*!
 * Domain type.
 * Use for performance level setting
 * Which hardware should be boost/decrease
 */
enum xrt_perf_domain
{
	XRT_PERF_DOMAIN_CPU = 1,
	XRT_PERF_DOMAIN_GPU = 2,
};

enum xrt_perf_sub_domain
{
	XRT_PERF_SUB_DOMAIN_COMPOSITING = 1,
	XRT_PERF_SUB_DOMAIN_RENDERING = 2,
	XRT_PERF_SUB_DOMAIN_THERMAL = 3
};

/*!
 * Performance level.
 */
enum xrt_perf_set_level
{
	XRT_PERF_SET_LEVEL_POWER_SAVINGS = 0,
	XRT_PERF_SET_LEVEL_SUSTAINED_LOW = 25,
	XRT_PERF_SET_LEVEL_SUSTAINED_HIGH = 50,
	XRT_PERF_SET_LEVEL_BOOST = 75,
};

/*!
 * Performance level.
 */
enum xrt_perf_notify_level
{
	XRT_PERF_NOTIFY_LEVEL_NORMAL = 0,
	XRT_PERF_NOTIFY_LEVEL_WARNING = 25,
	XRT_PERF_NOTIFY_LEVEL_IMPAIRED = 75,
};

/*!
 * Visibility mask, mirror of XrVisibilityMaskKHR
 *
 * @ingroup xrt_iface
 */
enum xrt_visibility_mask_type
{
	XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH = 1,
	XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH = 2,
	XRT_VISIBILITY_MASK_TYPE_LINE_LOOP = 3,
};


/*
 *
 * Inline functions
 *
 */

// @note We're not using U_LOG_RAW because we can't include u_logging.h inside xrt_defines.h without causing problems.
#if 0
#define XRT_REFERENCE_DEBUG_PRINT(ACTION, PTR, COUNT)                                                                  \
	do {                                                                                                           \
		int32_t __count = (COUNT);                                                                             \
		printf(#ACTION " %p to %u", (void *)(PTR), __count);                                                   \
		if (__count < 0) {                                                                                     \
			XRT_DEBUGBREAK();                                                                              \
		}                                                                                                      \
	} while (false)
#else
#define XRT_REFERENCE_DEBUG_PRINT(ACTION, PTR, COUNT)
#endif

/*!
 * Increment the reference, probably want @ref xrt_reference_inc_and_was_zero.
 *
 * @memberof xrt_reference
 * @ingroup xrt_iface
 */
static inline void
xrt_reference_inc(struct xrt_reference *xref)
{
	XRT_MAYBE_UNUSED int32_t count = xrt_atomic_s32_inc_return(&xref->count);

	XRT_REFERENCE_DEBUG_PRINT(Incremented, xref, count);
}

/*!
 * Decrement the reference, probably want @ref xrt_reference_dec_and_is_zero.
 *
 * @memberof xrt_reference
 * @ingroup xrt_iface
 */
static inline void
xrt_reference_dec(struct xrt_reference *xref)
{
	XRT_MAYBE_UNUSED int32_t count = xrt_atomic_s32_dec_return(&xref->count);

	XRT_REFERENCE_DEBUG_PRINT(Decremented, xref, count);
}

/*!
 * Increment the reference and return true if the value @p was zero.
 *
 * @memberof xrt_reference
 * @ingroup xrt_iface
 */
XRT_CHECK_RESULT static inline bool
xrt_reference_inc_and_was_zero(struct xrt_reference *xref)
{
	int32_t count = xrt_atomic_s32_inc_return(&xref->count);

	XRT_REFERENCE_DEBUG_PRINT(Incremented, xref, count);

	return count == 1;
}

/*!
 * Decrement the reference and return true if the value is now zero.
 *
 * @memberof xrt_reference
 * @ingroup xrt_iface
 */
XRT_CHECK_RESULT static inline bool
xrt_reference_dec_and_is_zero(struct xrt_reference *xref)
{
	int32_t count = xrt_atomic_s32_dec_return(&xref->count);

	XRT_REFERENCE_DEBUG_PRINT(Decremented, xref, count);

	return count == 0;
}

#ifdef __cplusplus
}
#endif
