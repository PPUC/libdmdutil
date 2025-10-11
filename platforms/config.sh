#!/bin/bash

set -e

LIBZEDMD_SHA=41c84011247dd55650b711e8680ae7088a86b822
LIBSERUM_SHA=e536fde7fb0d5a7cc0f77664243e4c6cc746b87c
LIBPUPDMD_SHA=124f45e5ddd59ceb339591de88fcca72f8c54612

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
