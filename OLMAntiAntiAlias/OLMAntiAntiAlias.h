#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AEFX_SuiteHelper.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "String_Utils.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"

#define OLMAAA_NAME       "OLM Anti Anti Alias"
#define OLMAAA_CATEGORY   "OLM Plug-ins"
#define OLMAAA_MATCH_NAME "OLM OLM Anti Anti Alias"  // doubled OLM is shipped; keep for old projects
#define OLMAAA_ABOUT_DESC "Get rid of anti-aliasing"
#define OLMAAA_VERSION    PF_VERSION(1, 1, 6, PF_Stage_DEVELOP, 0)  // 569344

// Parameter positions. Binary uu.id == position (input is 0). See spec.
enum OLMAAA_ParamPos {
    OLMAAA_INPUT = 0,
    OLMAAA_NUM_COLORS = 1,     // SLIDER 0..100, default 0
    OLMAAA_USE_AUTO = 2,       // CHECKBOX, default off
    OLMAAA_INITIALIZE = 3,     // BUTTON
    OLMAAA_ADD_AUTO = 4,       // BUTTON
    OLMAAA_CLEAR = 5,          // BUTTON
    OLMAAA_COLORS_GROUP = 6,   // GROUP_START
    OLMAAA_COLOR_FIRST = 7,    // COLOR 7..106
    OLMAAA_COLOR_COUNT = 100,
    OLMAAA_COLORS_GROUP_END = 107,
    OLMAAA_NUM_PARAMS = 108
};

// Per-pixel colour as the binary's PF_PixelFloat-style record: alpha, red, green, blue.
struct AAAColor {
    float a, r, g, b;
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
