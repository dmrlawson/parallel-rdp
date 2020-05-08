cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(ASTC-Encoder LANGUAGES CXX C)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set (X86 TRUE)
else()
    set (X86 FALSE)
endif()

if (CMAKE_COMPILER_IS_GNUCXX)
	if (X86)
		set(ASTC_CXX_FLAGS -msse2 -mfpmath=sse)
	endif()
elseif (MSVC)
	set(ASTC_CXX_FLAGS /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /MP /DNOMINMAX)
endif()

add_library(astc-encoder STATIC
	astc_averages_and_directions.cpp
	astc_block_sizes2.cpp
	astc_color_quantize.cpp
	astc_color_unquantize.cpp
	astc_compress_symbolic.cpp
	astc_decompress_symbolic.cpp
	astc_encoding_choice_error.cpp
	astc_find_best_partitioning.cpp
	astc_ideal_endpoints_and_weights.cpp
	astc_integer_sequence.cpp
	astc_kmeans_partitioning.cpp
	astc_partition_tables.cpp
	astc_image_load_store.cpp
	astc_percentile_tables.cpp
	astc_pick_best_endpoint_format.cpp
	astc_quantization.cpp
	astc_symbolic_physical.cpp
	astc_ktx_dds.cpp
	astc_weight_align.cpp
	astc_weight_quant_xfer_tables.cpp
	astc_compute_variance.cpp
	mathlib.cpp
	astc_toplevel.cpp
	astc_stb_tga.cpp
	softfloat.cpp)

target_include_directories(astc-encoder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(astc-encoder PUBLIC ${ASTC_CXX_FLAGS})
target_compile_definitions(astc-encoder PRIVATE ASTC_ENCODER_NO_MAIN)

