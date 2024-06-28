#!/bin/bash

set -e

CARGS_SHA=5949a20a926e902931de4a32adaad9f19c76f251
LIBZEDMD_SHA=f224864b507a3dedb7860ed99be4c275d79ca225
LIBSERUM_SHA=1b78576e0227851d7cc822b4098a4716a7cce34a
SOCKPP_SHA=e6c4688a576d95f42dd7628cefe68092f6c5cd0f
LIBPUPDMD_SHA=124f45e5ddd59ceb339591de88fcca72f8c54612
LIBFRAMEUTIL_SHA=30048ca23d41ca0a8f7d5ab75d3f646a19a90182

NUM_PROCS=$(sysctl -n hw.ncpu)

echo "Building libraries..."
echo "  CARGS_SHA: ${CARGS_SHA}"
echo "  LIBZEDMD_SHA: ${LIBZEDMD_SHA}"
echo "  LIBSERUM_SHA: ${LIBSERUM_SHA}"
echo "  SOCKPP_SHA: ${SOCKPP_SHA}"
echo "  LIBPUPDMD_SHA: ${LIBPUPDMD_SHA}"
echo "  LIBFRAMEUTIL_SHA: ${LIBFRAMEUTIL_SHA}"
echo ""

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo "Procs: ${NUM_PROCS}"
echo ""

rm -rf external
mkdir external
cd external

#
# build cargs and copy to external
#

curl -sL https://github.com/likle/cargs/archive/${CARGS_SHA}.zip -o cargs.zip
unzip cargs.zip
cd cargs-${CARGS_SHA}
cmake \
   -DCMAKE_SYSTEM_NAME=iOS \
   -DCMAKE_OSX_SYSROOT=iphonesimulator \
   -DCMAKE_OSX_ARCHITECTURES=arm64 \
   -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp include/cargs.h ../../third-party/include/
cp build/*.a ../../third-party/build-libs/ios-simulator/arm64/
cd ..

#
# build libzedmd and copy to external
#

curl -sL https://github.com/PPUC/libzedmd/archive/${LIBZEDMD_SHA}.zip -o libzedmd.zip
unzip libzedmd.zip
cd libzedmd-$LIBZEDMD_SHA
platforms/ios-simulator/arm64/external.sh
cmake \
   -DPLATFORM=ios-simulator \
   -DARCH=arm64 \
   -DBUILD_SHARED=OFF \
   -DBUILD_STATIC=ON \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/ZeDMD.h ../../third-party/include/
cp build/libzedmd.a ../../third-party/build-libs/ios-simulator/arm64/
cp -r test ../../
cd ..

#
# build libserum and copy to external
#

curl -sL https://github.com/jsm174/libserum/archive/${LIBSERUM_SHA}.zip -o libserum.zip
unzip libserum.zip
cd libserum-$LIBSERUM_SHA
cmake \
   -DPLATFORM=ios-simulator \
   -DARCH=arm64 \
   -DBUILD_SHARED=OFF \
   -DBUILD_STATIC=ON \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/serum.h ../../third-party/include/
cp src/serum-decode.h ../../third-party/include/
cp build/libserum.a ../../third-party/build-libs/ios-simulator/arm64/
cd ..

#
# build sockpp and copy to external
#

curl -sL https://github.com/fpagliughi/sockpp/archive/${SOCKPP_SHA}.zip -o sockpp.zip
unzip sockpp.zip
cd sockpp-$SOCKPP_SHA
cmake \
   -DSOCKPP_BUILD_SHARED=OFF \
   -DSOCKPP_BUILD_STATIC=ON \
   -DCMAKE_SYSTEM_NAME=iOS \
   -DCMAKE_OSX_SYSROOT=iphonesimulator \
   -DCMAKE_OSX_ARCHITECTURES=arm64 \
   -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp -r include/sockpp ../../third-party/include/
cp build/libsockpp.a ../../third-party/build-libs/ios-simulator/arm64/
cd ..

#
# build libpupdmd and copy to external
#

curl -sL https://github.com/ppuc/libpupdmd/archive/${LIBPUPDMD_SHA}.zip -o libpupdmd.zip
unzip libpupdmd.zip
cd libpupdmd-$LIBPUPDMD_SHA
cmake \
   -DPLATFORM=ios-simulator \
   -DARCH=arm64 \
   -DBUILD_SHARED=OFF \
   -DBUILD_STATIC=ON \
   -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
   -B build
cmake --build build -- -j${NUM_PROCS}
cp src/pupdmd.h ../../third-party/include/
cp build/libpupdmd.a ../../third-party/build-libs/ios-simulator/arm64/
cd ..

#
# copy libframeutil
#

curl -sL https://github.com/ppuc/libframeutil/archive/${LIBFRAMEUTIL_SHA}.zip -o libframeutil.zip
unzip libframeutil.zip
cd libframeutil-$LIBFRAMEUTIL_SHA
cp include/* ../../third-party/include
cd ..
