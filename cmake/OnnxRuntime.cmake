# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# ONNX Runtime as PINNED release archives, fetched at configure time and
# verified by SHA-256 before anything is extracted. Only the headers are used
# at build time: the provider loads the library at run time by absolute path
# from its own directory, so nothing links against it.
#
# Outputs:
#   OPENTRACK_ORT_INCLUDE_DIR   onnxruntime_c_api.h
#   OPENTRACK_ORT_RUNTIME_FILES files to place next to the provider binary
#   OPENTRACK_ORT_LICENSE_FILES / _NAMES  licence texts shipped with them
#
# -DOPENTRACK_DOWNLOAD_DIR=<dir> keeps the archives across build trees (CI caches it).
#
# Windows takes the plain CPU build (the release zip); a DirectML build is a
# follow-up together with the Media Foundation camera backend.

set(OPENTRACK_ORT_VERSION 1.31.0)

set(OPENTRACK_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads" CACHE PATH "Where pinned archives are downloaded")
set(_ort_root "${CMAKE_BINARY_DIR}/_deps/onnxruntime-${OPENTRACK_ORT_VERSION}")
set(_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${OPENTRACK_ORT_VERSION}")

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
	set(_name "onnxruntime-win-x64-${OPENTRACK_ORT_VERSION}")
	opentrack_fetch("${_ort_base}/${_name}.zip" "${_name}.zip"
		3958d8a44984160692c1b4ffd0de3cb48f8c39f9d34bb31e433db06e30b82195)
	set(_archive "${_name}.zip")
	set(_lib "onnxruntime.dll")
	set(_as "onnxruntime.dll")
elseif(APPLE)
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
		message(FATAL_ERROR "ONNX Runtime ${OPENTRACK_ORT_VERSION} ships macOS for arm64 only")
	endif()
	set(_name "onnxruntime-osx-arm64-${OPENTRACK_ORT_VERSION}")
	opentrack_fetch("${_ort_base}/${_name}.tgz" "${_name}.tgz"
		1031b36dd3bdaa8d976a6130d9cf2a05e9be320b93ae804f8427b91dc3432afc)
	set(_archive "${_name}.tgz")
	set(_lib "libonnxruntime.${OPENTRACK_ORT_VERSION}.dylib")
	set(_as "libonnxruntime.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
		message(FATAL_ERROR "Linux aarch64: not pinned yet")
	endif()
	set(_name "onnxruntime-linux-x64-${OPENTRACK_ORT_VERSION}")
	opentrack_fetch("${_ort_base}/${_name}.tgz" "${_name}.tgz"
		cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57)
	set(_archive "${_name}.tgz")
	set(_lib "libonnxruntime.so.${OPENTRACK_ORT_VERSION}")
	set(_as "libonnxruntime.so.1") # the SONAME
else()
	message(FATAL_ERROR "Unsupported platform for ONNX Runtime: ${CMAKE_SYSTEM_NAME}")
endif()

if(NOT EXISTS "${_ort_root}/.extracted")
	file(ARCHIVE_EXTRACT INPUT "${OPENTRACK_DOWNLOAD_DIR}/${_archive}" DESTINATION "${_ort_root}"
		PATTERNS "*${_name}/include/*" "*${_name}/lib/${_lib}" "*${_name}/LICENSE" "*${_name}/ThirdPartyNotices.txt")
	file(TOUCH "${_ort_root}/.extracted")
endif()
# Some archives prefix their top folder with "./".
set(_top "${_ort_root}/${_name}")
set(OPENTRACK_ORT_INCLUDE_DIR "${_top}/include")
configure_file("${_top}/lib/${_lib}" "${_ort_root}/runtime/${_as}" COPYONLY)
set(OPENTRACK_ORT_RUNTIME_FILES "${_ort_root}/runtime/${_as}")
set(OPENTRACK_ORT_LICENSE_FILES "${_top}/LICENSE")
set(OPENTRACK_ORT_LICENSE_NAMES "onnxruntime-LICENSE.txt")
if(EXISTS "${_top}/ThirdPartyNotices.txt")
	list(APPEND OPENTRACK_ORT_LICENSE_FILES "${_top}/ThirdPartyNotices.txt")
	list(APPEND OPENTRACK_ORT_LICENSE_NAMES "onnxruntime-ThirdPartyNotices.txt")
endif()

if(NOT EXISTS "${OPENTRACK_ORT_INCLUDE_DIR}/onnxruntime_c_api.h")
	message(FATAL_ERROR "ONNX Runtime headers missing under ${OPENTRACK_ORT_INCLUDE_DIR}")
endif()
message(STATUS "ONNX Runtime ${OPENTRACK_ORT_VERSION}: ${OPENTRACK_ORT_RUNTIME_FILES}")
