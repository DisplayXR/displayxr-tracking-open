# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# DisplayXR open camera tracking provider: DisplayXR-OpenTracking.{dll,so,dylib}
# (one export, xrtTrackingPluginNegotiate), ONNX Runtime next to it, the model
# pack manifest, a fake host + CTest driver, and otr-probe (a dev CLI).
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# -DOPENTRACK_TEST_MODELS_DIR=<dir> (scripts/fetch_models.py output, with the
# test photo under <dir>/images) enables the inference tests.

cmake_minimum_required(VERSION 3.22)
project(displayxr_tracking_open VERSION 0.1.0 LANGUAGES C)
if(APPLE)
	enable_language(OBJC)
endif()

# The runtime commit the vendored include/xrt/ headers were copied from (README "The ABI it builds
# against"). Metadata only: nothing compiles against it.
set(DXR_RUNTIME_GIT_TAG "v2.53.0-17-ge5d7ffa57")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # gnu11: POSIX (nanosleep, realpath, dladdr, getpwuid_r) without feature macros
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OPENTRACK_WERROR "Treat compiler warnings as errors (CI turns this on)" OFF)
option(OPENTRACK_BUILD_TESTS "Build the fake host, the unit tests and register the tests" ON)
option(OPENTRACK_BUILD_PROBE "Build otr-probe (dev CLI: the provider on a real camera)" ON)
set(OPENTRACK_TEST_MODELS_DIR "" CACHE PATH "scripts/fetch_models.py output dir (+ images/portrait.ppm)")

set(OPENTRACK_PROVIDER DisplayXR-OpenTracking)
set(OPENTRACK_PACK "${CMAKE_CURRENT_SOURCE_DIR}/packs/open-tracking-default.models.json")

include(cmake/OnnxRuntime.cmake)

function(opentrack_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /utf-8 /external:W0)
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
		if(OPENTRACK_WERROR)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
		if(OPENTRACK_WERROR)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()

# Pure + portable code shared by the provider and the tests.
set(OPENTRACK_CORE_SOURCES
	src/ot_eyes.c
	src/ot_face_geom.c
	src/ot_json.c
	src/ot_pack.c
	src/ot_platform.c
	src/ot_sha256.c
	src/ot_viewer.c)

# Capture: AVFoundation on macOS; Windows (Media Foundation) and Linux (V4L2) are follow-ups.
set(OPENTRACK_CAPTURE_SOURCES src/ot_capture_common.c src/ot_capture_file.c)
if(APPLE)
	list(APPEND OPENTRACK_CAPTURE_SOURCES src/ot_capture_avf.m)
else()
	list(APPEND OPENTRACK_CAPTURE_SOURCES src/ot_capture_stub.c)
endif()

# The provider: a loadable module exporting exactly one symbol. ONNX Runtime is
# NOT linked: it is loaded at run time by absolute path from this directory.
add_library(${OPENTRACK_PROVIDER} MODULE
	src/open_tracking.c
	src/ot_face_tracker.c
	src/ot_ort.c
	${OPENTRACK_CORE_SOURCES}
	${OPENTRACK_CAPTURE_SOURCES})
target_include_directories(${OPENTRACK_PROVIDER} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(${OPENTRACK_PROVIDER} SYSTEM PRIVATE ${OPENTRACK_ORT_INCLUDE_DIR})
set_target_properties(${OPENTRACK_PROVIDER} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden OBJC_VISIBILITY_PRESET hidden)
if(APPLE)
	set_target_properties(${OPENTRACK_PROVIDER} PROPERTIES SUFFIX ".dylib")
	set_source_files_properties(src/ot_capture_avf.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
	target_link_libraries(${OPENTRACK_PROVIDER} PRIVATE "-framework Foundation" "-framework AVFoundation"
		"-framework CoreMedia" "-framework CoreVideo")
endif()
if(WIN32)
	target_link_libraries(${OPENTRACK_PROVIDER} PRIVATE shell32 ole32 uuid)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(${OPENTRACK_PROVIDER} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(MSVC)
	# A provider DLL loads into a process whose CRT it does not control.
	set_property(TARGET ${OPENTRACK_PROVIDER} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
opentrack_warnings(${OPENTRACK_PROVIDER})

# Ship-together files next to the binary: ONNX Runtime and the pack manifest.
set(_ship ${OPENTRACK_ORT_RUNTIME_FILES} ${OPENTRACK_PACK})
add_custom_command(TARGET ${OPENTRACK_PROVIDER} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ship} $<TARGET_FILE_DIR:${OPENTRACK_PROVIDER}>
	VERBATIM)

# Install: the provider, ONNX Runtime, the pack manifest (the model blobs are added by the
# packaging step from scripts/fetch_models.py; never committed), licences, a manifest template.
install(TARGETS ${OPENTRACK_PROVIDER} LIBRARY DESTINATION . RUNTIME DESTINATION .)
install(FILES ${_ship} DESTINATION .)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION licenses)
list(LENGTH OPENTRACK_ORT_LICENSE_FILES _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE ${_last})
	list(GET OPENTRACK_ORT_LICENSE_FILES ${_i} _f)
	list(GET OPENTRACK_ORT_LICENSE_NAMES ${_i} _as)
	install(FILES ${_f} DESTINATION licenses RENAME ${_as})
endforeach()
install(FILES examples/manifest/100-open-tracking-tracking-provider.json.in DESTINATION examples)
set(OPENTRACK_INSTALL_MODELS_DIR "" CACHE PATH "Also install the ONNX blobs from this directory (packaging)")
if(OPENTRACK_INSTALL_MODELS_DIR)
	install(FILES "${OPENTRACK_INSTALL_MODELS_DIR}/face_detector.onnx" "${OPENTRACK_INSTALL_MODELS_DIR}/face_landmarks.onnx"
		DESTINATION .)
endif()

if(OPENTRACK_BUILD_TESTS OR OPENTRACK_BUILD_PROBE)
	add_library(opentrack-fakehost-lib STATIC tools/fake_host.c src/ot_platform.c src/ot_sha256.c)
	target_include_directories(opentrack-fakehost-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tools)
	if(WIN32)
		target_link_libraries(opentrack-fakehost-lib PUBLIC shell32 ole32 uuid)
	else()
		find_package(Threads REQUIRED)
		target_link_libraries(opentrack-fakehost-lib PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
	endif()
	opentrack_warnings(opentrack-fakehost-lib)
endif()

if(OPENTRACK_BUILD_PROBE)
	add_executable(otr-probe tools/otr_probe.c)
	target_link_libraries(otr-probe PRIVATE opentrack-fakehost-lib)
	add_dependencies(otr-probe ${OPENTRACK_PROVIDER})
	opentrack_warnings(otr-probe)
endif()

if(OPENTRACK_BUILD_TESTS)
	enable_testing()

	add_executable(open-tracking-unit-tests tests/unit_tests.c ${OPENTRACK_CORE_SOURCES})
	target_include_directories(open-tracking-unit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src)
	if(WIN32)
		target_link_libraries(open-tracking-unit-tests PRIVATE shell32 ole32 uuid)
	else()
		target_link_libraries(open-tracking-unit-tests PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
	endif()
	opentrack_warnings(open-tracking-unit-tests)

	foreach(_case one-euro pixel-dir distance roi decode viewer)
		add_test(NAME unit-${_case} COMMAND open-tracking-unit-tests ${_case})
	endforeach()
	add_test(NAME unit-pack COMMAND open-tracking-unit-tests pack ${OPENTRACK_PACK})

	# The vendored headers stay compilable on their own.
	add_library(opentrack-header-check OBJECT tests/header_check.c)
	target_include_directories(opentrack-header-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

	add_executable(open-tracking-fakehost tools/fakehost.c)
	target_link_libraries(open-tracking-fakehost PRIVATE opentrack-fakehost-lib)
	add_dependencies(open-tracking-fakehost ${OPENTRACK_PROVIDER})
	opentrack_warnings(open-tracking-fakehost)

	set(_bin $<TARGET_FILE:${OPENTRACK_PROVIDER}>)
	set(_fh $<TARGET_FILE:open-tracking-fakehost>)
	add_test(NAME negotiate COMMAND ${_fh} ${_bin} negotiate)
	add_test(NAME decline-disabled COMMAND ${_fh} ${_bin} decline-disabled --set ENABLE=0)
	add_test(NAME decline-models COMMAND ${_fh} ${_bin} decline-models --set MODEL_DIR=${CMAKE_BINARY_DIR}/no-models)
	add_test(NAME decline-camera COMMAND ${_fh} ${_bin} decline-camera
		--set DEVICE=file:${CMAKE_BINARY_DIR}/no-such-image.ppm)

	# Inference tests (the file camera): a blank frame and a public-domain face photo.
	if(OPENTRACK_TEST_MODELS_DIR)
		file(TO_CMAKE_PATH "${OPENTRACK_TEST_MODELS_DIR}" _models)
	else()
		set(_models "${CMAKE_BINARY_DIR}/no-test-models")
	endif()
	# A mid-grey 320x240 frame (no face in it), written by the fake host (--make-blank).
	set(_blank "${CMAKE_BINARY_DIR}/blank.ppm")
	add_test(NAME blank COMMAND ${_fh} ${_bin} blank --models ${_models} --make-blank --image ${_blank} --set EP=cpu)
	add_test(NAME face COMMAND ${_fh} ${_bin} face --models ${_models} --image ${_models}/images/portrait.ppm
		--set EP=cpu --set HFOV_DEG=40)
	set_tests_properties(blank face PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
	if(APPLE)
		add_test(NAME face-coreml COMMAND ${_fh} ${_bin} face --models ${_models}
			--image ${_models}/images/portrait.ppm --set EP=coreml --set HFOV_DEG=40
			--set CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache)
		set_tests_properties(face-coreml PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
	endif()

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_FOUND)
		add_test(NAME check-pack COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_pack.py
			${OPENTRACK_PACK})
	endif()
endif()
