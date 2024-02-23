#!/bin/bash

set -e

LIBZEDMD_SHA=d523c51a28258f169614cc818757f437c68dad34
LIBSERUM_SHA=b69d2b436bc93570a2e7e78d0946cd3c43f7aed5

echo "Building libraries..."
echo "  LIBZEDMD_SHA: ${LIBZEDMD_SHA}"
echo "  LIBSERUM_SHA: ${LIBSERUM_SHA}"
echo ""

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""

rm -rf external
mkdir external
cd external

#
# build libzedmd and copy to external
#

curl -sL https://github.com/PPUC/libzedmd/archive/${LIBZEDMD_SHA}.zip -o libzedmd.zip
unzip libzedmd.zip
cd libzedmd-$LIBZEDMD_SHA
cp src/ZeDMD.h ../../third-party/include
platforms/win/x64/external.sh
cmake -G "Visual Studio 17 2022" -DPLATFORM=win -DARCH=x64 -DBUILD_SHARED=ON -DBUILD_STATIC=OFF -B build
cmake --build build --config ${BUILD_TYPE}
cp third-party/include/libserialport.h ../../third-party/include
cp third-party/build-libs/win/x64/libserialport64.lib ../../third-party/build-libs/win/x64
cp third-party/runtime-libs/win/x64/libserialport64.dll ../../third-party/runtime-libs/win/x64
cp build/${BUILD_TYPE}/zedmd64.lib ../../third-party/build-libs/win/x64
cp build/${BUILD_TYPE}/zedmd64.dll ../../third-party/runtime-libs/win/x64
cp -r test ../../
cd ..

#
# build libserum and copy to external
#

curl -sL https://github.com/zesinger/libserum/archive/${LIBSERUM_SHA}.zip -o libserum.zip
unzip libserum.zip
cd libserum-$LIBSERUM_SHA
cp src/serum-decode.h ../../third-party/include
cmake -G "Visual Studio 17 2022" -DPLATFORM=win -DARCH=x64 -DBUILD_SHARED=ON -DBUILD_STATIC=OFF -B build
cmake --build build --config ${BUILD_TYPE}
cp build/${BUILD_TYPE}/serum64.lib ../../third-party/build-libs/win/x64
cp build/${BUILD_TYPE}/serum64.dll ../../third-party/runtime-libs/win/x64
cd ..