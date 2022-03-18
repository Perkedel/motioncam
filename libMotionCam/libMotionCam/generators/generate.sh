#!/bin/bash
set -euxo pipefail

if [ -z "${HALIDE_PATH+x}" ]
then
	HALIDE_PATH="../../thirdparty/halide"
fi

if [ ! -d ${HALIDE_PATH} ] 
then
    echo "Halide is missing. Run setupenv.sh first"
    exit 1
fi

if [[ "$OSTYPE" == "darwin"* ]]; then
	export DYLD_LIBRARY_PATH=${HALIDE_PATH}/lib
else
	export LD_LIBRARY_PATH=${HALIDE_PATH}/lib
fi

rm -rf tmp
mkdir -p tmp

g++ DenoiseGenerator.cpp ${HALIDE_PATH}/share/tools/GenGen.cpp -g -o3 -std=c++17 -Wall -pedantic -I ${HALIDE_PATH}/include -L ${HALIDE_PATH}/lib -lHalide -lpthread -ldl -o ./tmp/denoise_generator
g++ PostProcessGenerator.cpp ${HALIDE_PATH}/share/tools/GenGen.cpp -g -o3 -std=c++17 -Wall -pedantic -I ${HALIDE_PATH}/include -L ${HALIDE_PATH}/lib -lHalide -lpthread -ldl -o ./tmp/postprocess_generator

function build_denoise() {
	TARGET=$1
	ARCH=$2
	FLAGS="no_runtime"

	echo "[$ARCH] Building denoise_generator_3x3"
	./tmp/denoise_generator -g denoise_generator -f fuse_denoise_3x3 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} window=3

	echo "[$ARCH] Building denoise_generator_5x5"
	./tmp/denoise_generator -g denoise_generator -f fuse_denoise_5x5 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} window=5

	echo "[$ARCH] Building denoise_generator_7x7"
	./tmp/denoise_generator -g denoise_generator -f fuse_denoise_7x7 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} window=7

	echo "[$ARCH] Building forward_transform_generator"
	./tmp/denoise_generator -g forward_transform_generator -f forward_transform -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} input.type=uint16 levels=4

	echo "[$ARCH] Building fuse_image_generator"
	./tmp/denoise_generator -g fuse_image_generator -f fuse_image -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} input.type=uint16 reference.size=4 reference.type=float32 intermediate.size=4 intermediate.type=float32

	echo "[$ARCH] Building inverse_transform_generator"
	./tmp/denoise_generator -g inverse_transform_generator -f inverse_transform -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} input.size=4
}

function build_postprocess() {
	TARGET=$1
	ARCH=$2
	FLAGS="no_runtime"

	echo "[$ARCH] Building stats_generator"
	./tmp/postprocess_generator -g stats_generator -f generate_stats -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building measure_noise_generator"
	./tmp/postprocess_generator -g measure_noise_generator -f measure_noise -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building build_bayer_generator"
	./tmp/postprocess_generator -g build_bayer_generator -f build_bayer -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building build_bayer_generator2"
	./tmp/postprocess_generator -g build_bayer_generator2 -f build_bayer2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building hdr_mask_generator"
	./tmp/postprocess_generator -g hdr_mask_generator -f hdr_mask -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building linear_image_generator"
	./tmp/postprocess_generator -g linear_image_generator -f linear_image -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building measure_image_generator"
	./tmp/postprocess_generator -g measure_image_generator -f measure_image -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building generate_edges_generator"
	./tmp/postprocess_generator -g generate_edges_generator -f generate_edges -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building deinterleave_raw_generator"
	./tmp/postprocess_generator -g deinterleave_raw_generator -f deinterleave_raw -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building postprocess_generator"
	./tmp/postprocess_generator -g postprocess_generator -f postprocess -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building fast_preview_generator"
	./tmp/postprocess_generator -g fast_preview_generator -f fast_preview -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building fast_preview_generator2"
	./tmp/postprocess_generator -g fast_preview_generator2 -f fast_preview2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS}

	echo "[$ARCH] Building preview_generator2 rotation=0"
	./tmp/postprocess_generator -g preview_generator -f preview_landscape2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=0 tonemap_levels=9 downscale_factor=2 enable_sharpen=true pop_radius=7

	echo "[$ARCH] Building preview_generator2 rotation=90"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_portrait2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=90 tonemap_levels=9 downscale_factor=2 enable_sharpen=true pop_radius=7

	echo "[$ARCH] Building preview_generator2 rotation=-90"
	./tmp/postprocess_generator -g preview_generator -f preview_portrait2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=-90 tonemap_levels=9 downscale_factor=2 enable_sharpen=true pop_radius=7

	echo "[$ARCH] Building preview_generator2 rotation=180"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_landscape2 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=180 tonemap_levels=9 downscale_factor=2 enable_sharpen=true pop_radius=7

	echo "[$ARCH] Building preview_generator4 rotation=0"
	./tmp/postprocess_generator -g preview_generator -f preview_landscape4 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=0 tonemap_levels=8 downscale_factor=4 enable_sharpen=true pop_radius=3

	echo "[$ARCH] Building preview_generator4 rotation=90"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_portrait4 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=90 tonemap_levels=8 downscale_factor=4 enable_sharpen=true pop_radius=3

	echo "[$ARCH] Building preview_generator4 rotation=-90"
	./tmp/postprocess_generator -g preview_generator -f preview_portrait4 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=-90 tonemap_levels=8 downscale_factor=4 enable_sharpen=true pop_radius=3

	echo "[$ARCH] Building preview_generator4 rotation=180"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_landscape4 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=180 tonemap_levels=8 downscale_factor=4 enable_sharpen=true pop_radius=3

	echo "[$ARCH] Building preview_generator8 rotation=0"
	./tmp/postprocess_generator -g preview_generator -f preview_landscape8 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=0 tonemap_levels=7 downscale_factor=8 enable_sharpen=false pop_radius=3

	echo "[$ARCH] Building preview_generator8 rotation=90"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_portrait8 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=90 tonemap_levels=7 downscale_factor=8 enable_sharpen=false pop_radius=3

	echo "[$ARCH] Building preview_generator8 rotation=-90"
	./tmp/postprocess_generator -g preview_generator -f preview_portrait8 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=-90 tonemap_levels=7 downscale_factor=8 enable_sharpen=false

	echo "[$ARCH] Building preview_generator8 rotation=180"
	./tmp/postprocess_generator -g preview_generator -f preview_reverse_landscape8 -e static_library,h -o ../halide/${ARCH} target=${TARGET}-${FLAGS} rotation=180 tonemap_levels=7 downscale_factor=8 enable_sharpen=false
}

function build_runtime() {
	TARGET=$1
	ARCH=$2

	echo "[$ARCH] Building halide_runtime_base"
	./tmp/postprocess_generator -r halide_runtime -e static_library,h -o ../halide/${ARCH} target=${TARGET}

	mv ../halide/${ARCH}/halide_runtime.a ../halide/${ARCH}/halide_runtime_host.a

	# echo "[$ARCH] Building halide_runtime_opencl"
	# ./tmp/postprocess_generator -r halide_runtime -e static_library,h -o ../halide/${ARCH} target=${TARGET}-opencl-cl_half

	# mv ../halide/${ARCH}/halide_runtime.a ../halide/${ARCH}/halide_runtime_opencl.a
}

mkdir -p ../halide/host

build_denoise host host
build_postprocess host host
build_runtime host host

mkdir -p ../halide/arm64-v8a

build_denoise arm-64-android arm64-v8a
build_postprocess arm-64-android arm64-v8a
build_runtime arm-64-android arm64-v8a
