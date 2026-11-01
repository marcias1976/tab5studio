#pragma once

#include <stdbool.h>

// Language of the device texts. The default comes from the build (Tab5 Studio writes studio/lang.h with LANG_EN);
// it can be changed in the settings (config().lang) and takes effect after a restart.
#if __has_include("studio/lang.h")
#include "studio/lang.h"
#endif
#ifndef LANG_EN
#define LANG_EN 0
#endif
#define LANG_DEFAULT_EN LANG_EN

extern bool g_lang_en;  // set by config_load before anything is built
#define TR(pl, en) (g_lang_en ? (en) : (pl))
#define LANG_CODE (g_lang_en ? "en" : "pl")
