#pragma once

// Which build this program is, written by Studio into studio/build.h:
//   STUDIO_BUILD       the build ID, e.g. "20261101-100136-b977" (the last 4 characters only keep IDs unique);
//                      /api/state "build", matched by Studio against its build list
//   STUDIO_BUILD_NAME  the same build for people: its date and time, e.g. "01.11.2026 10:01"
//   STUDIO_BUILD_NOTE  what the build is about, typed in Studio ("" = none)
#if __has_include("studio/build.h")
#include "studio/build.h"
#endif
#ifndef STUDIO_BUILD
#define STUDIO_BUILD "dev"
#endif
#ifndef STUDIO_BUILD_NAME
#define STUDIO_BUILD_NAME "dev"
#endif
#ifndef STUDIO_BUILD_NOTE
#define STUDIO_BUILD_NOTE ""
#endif
