#!/bin/bash

set -e

LIBZEDMD_SHA=41c84011247dd55650b711e8680ae7088a86b822
LIBSERUM_SHA=3dee03bf540cfc8c9bf15fda1a71639caadc274e
LIBPUPDMD_SHA=124f45e5ddd59ceb339591de88fcca72f8c54612

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
