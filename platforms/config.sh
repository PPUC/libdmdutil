#!/bin/bash

set -e

LIBZEDMD_SHA=0e0fbbfbd485dfef020fcf31e9410dbbb8e90bf2
LIBSERUM_SHA=6a0188b286622f34e0ff71bd4698ea9e9404c301
LIBPUPDMD_SHA=124f45e5ddd59ceb339591de88fcca72f8c54612

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""