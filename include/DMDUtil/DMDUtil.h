#pragma once

#define DMDUTIL_VERSION_MAJOR 0 // X Digits
#define DMDUTIL_VERSION_MINOR 1 // Max 2 Digits
#define DMDUTIL_VERSION_PATCH 0 // Max 2 Digits

#define _DMDUTIL_STR(x) #x
#define DMDUTIL_STR(x) _DMDUTIL_STR(x)

#define DMDUTIL_VERSION DMDUTIL_STR(DMDUTIL_VERSION_MAJOR) "." DMDUTIL_STR(DMDUTIL_VERSION_MINOR) "." DMDUTIL_STR(DMDUTIL_VERSION_PATCH)
#define DMDUTIL_MINOR_VERSION DMDUTIL_STR(DMDUTIL_VERSION_MAJOR) "." DMDUTIL_STR(DMDUTIL_VERSION_MINOR)

#include "DMD.h"
#include "Config.h"