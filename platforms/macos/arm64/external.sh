#!/bin/bash

set -e

source ./platforms/config.sh

echo "Building libraries..."
echo "  LIBZEDMD_SHA: ${LIBZEDMD_SHA}"
echo "  LIBSERUM_SHA: ${LIBSERUM_SHA}"
echo "  LIBPUPDMD_SHA: ${LIBPUPDMD_SHA}"
echo ""

NUM_PROCS=$(sysctl -n hw.ncpu)

rm -rf external
mkdir external
cd external

#
# build libzedmd and copy to external
#

curl -sL https://github.com/PPUC/libzedmd/archive/${LIBZEDMD_SHA}.tar.gz -o libzedmd-${LIBZEDMD_SHA}.tar.gz
tar xzf libzedmd-${LIBZEDMD_SHA}.tar.gz
mv libzedmd-${LIBZEDMD_SHA} libzedmd
cd libzedmd
BUILD_TYPE=${BUILD_TYPE} platforms/macos/arm64/external.sh
cmake \
   -DPLATFORM=macos \
   -DARCH=arm64 \
   -DBUILD_SHARED=ON \
   -DBUILD_STATIC=OFF \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/ZeDMD.h ../../third-party/include/
cp third-party/include/libserialport.h ../../third-party/include/
cp third-party/include/cargs.h ../../third-party/include/
cp -r third-party/include/komihash ../../third-party/include/
cp -r third-party/include/sockpp ../../third-party/include/
cp third-party/include/FrameUtil.h ../../third-party/include/
cp third-party/runtime-libs/macos/arm64/libcargs.dylib ../../third-party/runtime-libs/macos/arm64/
cp -a third-party/runtime-libs/macos/arm64/libserialport.{dylib,*.dylib} ../../third-party/runtime-libs/macos/arm64/
cp -a third-party/runtime-libs/macos/arm64/libsockpp.{dylib,*.dylib} ../../third-party/runtime-libs/macos/arm64/
cp -a build/libzedmd.{dylib,*.dylib} ../../third-party/runtime-libs/macos/arm64/
cp -r test ../../
cd ..

#
# build libserum and copy to external
#

curl -sL https://github.com/PPUC/libserum_concentrate/archive/${LIBSERUM_SHA}.tar.gz -o libserum-${LIBSERUM_SHA}.tar.gz
tar xzf libserum-${LIBSERUM_SHA}.tar.gz
mv libserum_concentrate-${LIBSERUM_SHA} libserum
cd libserum
cmake \
   -DPLATFORM=macos \
   -DARCH=arm64 \
   -DBUILD_SHARED=ON \
   -DBUILD_STATIC=OFF \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/serum.h ../../third-party/include/
cp src/serum-decode.h ../../third-party/include/
cp -a build/libserum.{dylib,*.dylib} ../../third-party/runtime-libs/macos/arm64/
cd ..

#
# build libpupdmd and copy to external
#

curl -sL https://github.com/ppuc/libpupdmd/archive/${LIBPUPDMD_SHA}.tar.gz -o libpupdmd-${LIBPUPDMD_SHA}.tar.gz
tar xzf libpupdmd-${LIBPUPDMD_SHA}.tar.gz
mv libpupdmd-${LIBPUPDMD_SHA} libpupdmd
cd libpupdmd
cmake \
   -DPLATFORM=macos \
   -DARCH=arm64 \
   -DBUILD_SHARED=ON \
   -DBUILD_STATIC=OFF \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/pupdmd.h ../../third-party/include/
cp -a build/libpupdmd.{dylib,*.dylib} ../../third-party/runtime-libs/macos/arm64/
cd ..
