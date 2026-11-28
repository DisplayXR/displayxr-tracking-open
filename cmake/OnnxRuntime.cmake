# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# ONNX Runtime as a PINNED release package, fetched at configure time and
# verified by SHA-256 before anything is extracted. Only the headers are used
# at build time: the provider loads the library at run time by absolute path
# from its own directory, so nothing links against it.
#
# The source is Microsoft's official NuGet package `Microsoft.ML.OnnxRuntime`
# (one package carries the headers and the native library of every platform,
# and it is signed by Microsoft). The GitHub release archives this used before
# are no longer downloadable.
#
# Outputs:
#   OPENTRACK_ORT_INCLUDE_DIR   onnxruntime_c_api.h
#   OPENTRACK_ORT_RUNTIME_FILES files to place next to the provider binary
#   OPENTRACK_ORT_LICENSE_FILES / _NAMES  licence texts shipped with them
#
# -DOPENTRACK_DOWNLOAD_DIR=<dir> keeps the package across build trees (CI caches it).
#
# Windows takes the plain CPU build; a DirectML build is not used (its package
# stopped at an older release).

set(OPENTRACK_ORT_VERSION 1.31.0)
set(OPENTRACK_ORT_NUPKG_SHA256 7e35fdb4f5adb62f01e60798e2e5ed05108f5fbbe1cca03fbcc37903cf7c5e3b)

set(OPENTRACK_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads" CACHE PATH "Where pinned archives are downloaded")
set(_ort_root "${CMAKE_BINARY_DIR}/_deps/onnxruntime-${OPENTRACK_ORT_VERSION}")
set(_ort_url "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${OPENTRACK_ORT_VERSION}/microsoft.ml.onnxruntime.${OPENTRACK_ORT_VERSION}.nupkg")

# file(DOWNLOAD) fails the configure on a hash mismatch. The network may be flaky: retry.
function(opentrack_fetch url file sha256)
	set(_dst "${OPENTRACK_DOWNLOAD_DIR}/${file}")
	if(EXISTS "${_dst}")
		file(SHA256 "${_dst}" _have)
		if(_have STREQUAL "${sha256}")
			return()
		endif()
		file(REMOVE "${_dst}")
	endif()
	foreach(_try RANGE 1 5)
		message(STATUS "Downloading ${file} (attempt ${_try})")
		file(DOWNLOAD "${url}" "${_dst}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS _st)
		list(GET _st 0 _code)
		if(_code EQUAL 0)
			return()
		endif()
		file(REMOVE "${_dst}")
	endforeach()
	message(FATAL_ERROR "Download of ${url} failed: ${_st}")
endfunction()

if(WIN32)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
		message(FATAL_ERROR "Windows on Arm: not pinned yet")
	endif()
	set(_rid "win-x64")
	set(_lib "onnxruntime.dll")
	set(_as "onnxruntime.dll")
elseif(APPLE)
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
		message(FATAL_ERROR "ONNX Runtime ${OPENTRACK_ORT_VERSION} ships macOS for arm64 only")
	endif()
	set(_rid "osx-arm64")
	set(_lib "libonnxruntime.dylib")
	set(_as "libonnxruntime.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
		message(FATAL_ERROR "Linux aarch64: not pinned yet")
	endif()
	set(_rid "linux-x64")
	set(_lib "libonnxruntime.so")
	set(_as "libonnxruntime.so.1") # the SONAME
else()
	message(FATAL_ERROR "Unsupported platform for ONNX Runtime: ${CMAKE_SYSTEM_NAME}")
endif()

set(_archive "microsoft.ml.onnxruntime.${OPENTRACK_ORT_VERSION}.zip") # a .nupkg is a zip
opentrack_fetch("${_ort_url}" "${_archive}" ${OPENTRACK_ORT_NUPKG_SHA256})

set(_stamp "${_ort_root}/.extracted-${_rid}")
if(NOT EXISTS "${_stamp}")
	file(ARCHIVE_EXTRACT INPUT "${OPENTRACK_DOWNLOAD_DIR}/${_archive}" DESTINATION "${_ort_root}"
		PATTERNS "build/native/include/*" "runtimes/${_rid}/native/${_lib}" "LICENSE" "ThirdPartyNotices.txt")
	file(TOUCH "${_stamp}")
endif()
set(OPENTRACK_ORT_INCLUDE_DIR "${_ort_root}/build/native/include")
configure_file("${_ort_root}/runtimes/${_rid}/native/${_lib}" "${_ort_root}/runtime/${_as}" COPYONLY)
set(OPENTRACK_ORT_RUNTIME_FILES "${_ort_root}/runtime/${_as}")
set(OPENTRACK_ORT_LICENSE_FILES "${_ort_root}/LICENSE")
set(OPENTRACK_ORT_LICENSE_NAMES "onnxruntime-LICENSE.txt")
if(EXISTS "${_ort_root}/ThirdPartyNotices.txt")
	list(APPEND OPENTRACK_ORT_LICENSE_FILES "${_ort_root}/ThirdPartyNotices.txt")
	list(APPEND OPENTRACK_ORT_LICENSE_NAMES "onnxruntime-ThirdPartyNotices.txt")
endif()

if(NOT EXISTS "${OPENTRACK_ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
	message(FATAL_ERROR "ONNX Runtime headers missing under ${OPENTRACK_ORT_INCLUDE_DIR}")
endif()
message(STATUS "ONNX Runtime ${OPENTRACK_ORT_VERSION}: ${OPENTRACK_ORT_RUNTIME_FILES}")
