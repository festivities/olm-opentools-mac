#pragma once
// OLMDirectionalBlur – macOS port of OLM Directional Blur 1.1.1 (Windows .aex).
// 1:1 port: identical params, defaults, SmartRender CPU pipeline (8/16/32-bit).
// Windows original: entryPointFunc, VS2019, OpenMP, no GPU.

#include "AEConfig.h"
#include "entry.h"
#include "AEFX_SuiteHelper.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "String_Utils.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"

#define OLMDB_NAME "OLMDirectionalBlur"
#define OLMDB_MATCH_NAME "OLM Directional Blur"
#define OLMDB_CATEGORY "OLM Plug-ins"
#define OLMDB_DESCRIPTION "\nOLM Directional Blur — anisotropic directional blur ignoring transparent pixels."
#define OLMDB_URL "https://github.com/olm-digi/olm-opentools"

#define OLMDB_MAJOR_VERSION 1
#define OLMDB_MINOR_VERSION 1
#define OLMDB_BUG_VERSION 1
#define OLMDB_STAGE_VERSION PF_Stage_DEVELOP
#define OLMDB_BUILD_VERSION 0

// Param indices (params[0] = input layer). Order/decoding from Windows binary.
enum {
    OLMDB_INPUT = 0,
    OLMDB_ANGLE = 1,          // ANGLE, dflt 0
    OLMDB_BRIGHTNESS,         // FLOAT_SLIDER 0..10, slider 0..2, dflt 1.0
    OLMDB_SIZE_VAR,           // FIX_SLIDER % 0..100, dflt 0
    OLMDB_FRONT_TOPIC,        // GROUP_START "Front Blur Parameters"
    OLMDB_FRONT_BLUR,         // SLIDER 0..4000, dflt 0
    OLMDB_FRONT_FADE,         // SLIDER 0..100, dflt 0 ("Alpha Fade")
    OLMDB_FRONT_TAIL,         // FIX_SLIDER % 0..100, dflt 0 ("Sharp Tail")
    OLMDB_FRONT_END,          // GROUP_END
    OLMDB_BACK_TOPIC,         // GROUP_START "Back Blur Parameters"
    OLMDB_BACK_BLUR,          // SLIDER 0..4000, dflt 0
    OLMDB_BACK_FADE,          // SLIDER 0..100, dflt 0
    OLMDB_BACK_TAIL,          // FIX_SLIDER % 0..100, dflt 0
    OLMDB_BACK_END,           // GROUP_END
    OLMDB_NOISE_TOPIC,        // GROUP_START "Noise Parameters"
    OLMDB_NOISE_VAR,          // FIX_SLIDER % 0..100, dflt 0 ("Noise Variation")
    OLMDB_NOISE_TYPE,         // POPUP "Smooth | Block | Layer", dflt 1 (Smooth)
    OLMDB_NOISE_LAYER,        // LAYER "Noise Layer"
    OLMDB_SEED,               // SLIDER 1..1000, dflt 1
    OLMDB_OFFSET,             // ANGLE, dflt 0
    OLMDB_THICKNESS,          // FLOAT_SLIDER 1..100, dflt 10.0
    OLMDB_NOISE_END,          // GROUP_END
    OLMDB_NUM_PARAMS
};

enum {
    OLMDB_NOISE_SMOOTH = 1,
    OLMDB_NOISE_BLOCK = 2,
    OLMDB_NOISE_LAYER = 3
};

// Checked-out params, mirrors Windows pre-render struct layout semantically.
typedef struct {
    float angle_rad;      // (trunc(deg) + 90) / 180 * PI
    float brightness;     // gain multiplier
    float size_var;       // 0..1
    A_long front_blur;    // px 0..4000
    A_long front_fade;    // 0..100
    float front_tail;     // 0..1
    A_long back_blur;
    A_long back_fade;
    float back_tail;
    float noise_var;      // 0..1
    A_long noise_type;    // 1..3
    A_long seed;
    float offset;         // trunc(deg) / 36
    float thickness;
} OLMDBParams;

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
