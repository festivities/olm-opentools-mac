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

#define OLMS2_NAME        "OLM Smoother v2"
#define OLMS2_CATEGORY    "OLM Plug-ins"
#define OLMS2_PIPL_NAME   "OLM Smoother v2"
#define OLMS2_MATCH_NAME  "OLM Smoother v2"
// Literal "2" and "1" (not parsed from the version), as in the binary strings.
#define OLMS2_ABOUT       "OLM Smoother v2 2.2\rSmooth images."

// 2.2.0 = PF_VERSION(2,2,0,DEVELOP,0) = 1114112: PiPL AE_Effect_Version and the
// about format args (2, 2) of the 2026-11-20 Windows binary (0x18000AD30).
#define OLMS2_MAJOR_VERSION 2
#define OLMS2_MINOR_VERSION 2
#define OLMS2_BUG_VERSION   0

// PF_ParamDef.uu.id values from the Windows binary (position is the checkout
// index; uu.id is identity only).
enum OLMS2_ParamId {
    OLMS2_ID_ENABLE_KEY   = 1,
    OLMS2_ID_KEY_COLOR    = 2,
    OLMS2_ID_INVERT_KEY   = 15,
    OLMS2_ID_SMOOTHNESS   = 3,
    OLMS2_ID_EXTRA_SMOOTH = 4,
    OLMS2_ID_SMOOTH_RANGE = 5,
    OLMS2_ID_VERSION      = 6,
    OLMS2_ID_GAMMA_MODE   = 7,
    OLMS2_ID_GAMMA_VALUE  = 8,
    OLMS2_ID_GAMMA_COUNT  = 9,
    OLMS2_ID_GAMMA_COLOR  = 10  // 10..14 for the five pickers
};

// params[0] is the input layer. These are parameter positions (checkout
// indices), not uu.id.
enum OLMS2_ParamPosition {
    OLMS2_INPUT         = 0,
    OLMS2_ENABLE_KEY    = 1,
    OLMS2_KEY_COLOR     = 2,
    OLMS2_INVERT_KEY    = 3,
    OLMS2_SMOOTHNESS    = 4,
    OLMS2_EXTRA_SMOOTH  = 5,
    OLMS2_SMOOTH_RANGE  = 6,
    OLMS2_VERSION       = 7,
    OLMS2_GAMMA_MODE    = 8,
    OLMS2_GAMMA_VALUE   = 9,
    OLMS2_GAMMA_COUNT   = 10,
    OLMS2_GAMMA_COLOR_0 = 11  // 11..15
};

constexpr int OLMS2_NUM_PARAMS = 16;
constexpr int OLMS2_MAX_GAMMA_COLORS = 5;

// Gamma Correction popup values (position 8).
enum OLMS2_GammaMode {
    OLMS2_GAMMA_NONE = 1,
    OLMS2_GAMMA_COLORS = 2,
    OLMS2_GAMMA_ALL = 3
};

// Smoother Version popup value (1-based) maps to the binary's color-space index
// (0x180001C30 popup 1/2/3 -> 0/1/2, checkout 0x180005C60 at ctx+12).
enum OLMS2_Version {
    OLMS2_V1_SRGB = 0,     // "v1 (sRGB)"   popup 1 (no decode/encode)
    OLMS2_V2_LINEAR = 1,   // "v2 (Linear)" popup 2 (default)
    OLMS2_V2_OKLAB = 2     // "v2 (Oklab)"  popup 3 (no Gamma Correction)
};

typedef struct {
    bool enable_key;
    bool invert_key;
    float key_color[3];        // R,G,B (sRGB, as parameterized)
    int smoothness;            // raw slider ints, no downsample scaling
    int extra_smooth;
    // Smooth Range is a FLOAT slider in 2.2 (PF_Param_FLOAT_SLIDER). Binary ctx+40
    // = float(value) * 0.01f; the edge test adds 0.001f to it (0x18000C1D0).
    float smooth_threshold;
    int version;               // OLMS2_Version (0 sRGB, 1 linear, 2 Oklab)
    int gamma_mode;            // popup value 1/2/3; forced to NONE for Oklab
    float gamma_value;         // float(fs_d.value)
    int gamma_count;
    float gamma_colors[OLMS2_MAX_GAMMA_COLORS][3];
    A_long bitdepth;
} OLMS2Params;

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
