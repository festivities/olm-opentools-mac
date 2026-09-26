#pragma once
// OLMDirectionalBlur - macOS port of OLM Directional Blur 1.1.1 (Windows .aex).
//
// Ported 1:1 from the Windows x64 binary (PE64, entryPointFunc) after full
// decompilation. See .agents/AGENTS.md for the reverse-engineering notes.
//
// Reference: OLM OpenTools (OLM Digital). Windows original compiled with
// MSVC / VS2019; classic SmartRender CPU effect (no GPU), internal float
// working canvases, 8/16/32-bit support.

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

// Identity (strings decoded from the binary).
#define OLMDB_NAME            "OLMDirectionalBlur"          // About + AEGP registration name
#define OLMDB_CATEGORY        "OLM Plug-ins"                // PiPL category
#define OLMDB_PIPL_NAME       "OLM DirectionalBlur"         // PiPL name (no space, as shipped)
#define OLMDB_MATCH_NAME      "OLM Directional Blur"        // PiPL match name (with space)
#define OLMDB_DESCRIPTION     "Empty sample for you to mess around with"
#define OLMDB_URL             "https://www.olm.co.jp"

// Version: binary reports PF_VERSION(1,1,1,PF_Stage_DEVELOP,0) -> 559104.
#define OLMDB_MAJOR_VERSION   1
#define OLMDB_MINOR_VERSION   1
#define OLMDB_BUG_VERSION     1
#define OLMDB_STAGE_VERSION   PF_Stage_DEVELOP
#define OLMDB_BUILD_VERSION   0

// Parameter indices (params[0] = input layer). Decoded from ParamsSetup.
enum {
    OLMDB_INPUT = 0,
    OLMDB_ANGLE,         // 1  ANGLE         default 0
    OLMDB_BRIGHTNESS,    // 2  FLOAT_SLIDER  0..10 (slider 0..2), default 1.0, prec 2
    OLMDB_SIZE_VAR,      // 3  FIX_SLIDER %  0..100, default 0
    OLMDB_FRONT_TOPIC,   // 4  GROUP_START  "Front Blur Parameters"
    OLMDB_FRONT_BLUR,    // 5  SLIDER 0..4000 default 0
    OLMDB_FRONT_FADE,    // 6  SLIDER 0..100  default 0  ("Alpha Fade")
    OLMDB_FRONT_TAIL,    // 7  FIX_SLIDER %  0..100 default 0 ("Sharp Tail")
    OLMDB_FRONT_END,     // 8  GROUP_END
    OLMDB_BACK_TOPIC,    // 9  GROUP_START  "Back Blur Parameters"
    OLMDB_BACK_BLUR,     // 10 SLIDER 0..4000 default 0
    OLMDB_BACK_FADE,     // 11 SLIDER 0..100  default 0
    OLMDB_BACK_TAIL,     // 12 FIX_SLIDER %  0..100 default 0
    OLMDB_BACK_END,      // 13 GROUP_END
    OLMDB_NOISE_TOPIC,   // 14 GROUP_START  "Noise Parameters"
    OLMDB_NOISE_VAR,     // 15 FIX_SLIDER %  0..100 default 0 ("Noise Variation")
    OLMDB_NOISE_TYPE,    // 16 POPUP "Smooth | Block | Layer" default 1 (num_choices=2 in binary)
    OLMDB_NOISE_LAYER,   // 17 LAYER default none
    OLMDB_SEED,          // 18 SLIDER 1..1000 default 1
    OLMDB_OFFSET,        // 19 ANGLE default 0
    OLMDB_THICKNESS,     // 20 FLOAT_SLIDER 1..100 default 10.0, prec 2
    OLMDB_NOISE_END,     // 21 GROUP_END
    OLMDB_NUM_PARAMS
};

// Popup choices as stored in the param.
enum {
    OLMDB_POPUP_SMOOTH = 1,
    OLMDB_POPUP_BLOCK  = 2,
    OLMDB_POPUP_LAYER  = 3
};

// Internal noise dispatch (mode field, matches Windows scratch offset +32).
enum {
    OLMDB_MODE_OFF       = 1,   // noise variation == 0
    OLMDB_MODE_LAYER     = 2,   // popup == Layer
    OLMDB_MODE_GENERATED = 3    // popup == Smooth or Block
};

// Checked-out parameter values (Windows pre-render struct semantics).
typedef struct {
    double angle_rad;      // (trunc(deg) + 90) / 180 * PI, computed in double
    float  brightness;     // 0..10
    float  size_var;       // 0..1   (fixed / 100)
    int    front_blur;     // 0..4000 (scaled by downsample)
    int    front_fade;     // 0..100
    float  front_tail;     // 0..1
    int    back_blur;
    int    back_fade;
    float  back_tail;
    float  noise_var;      // 0..1
    int    noise_type;     // 1..3 popup value
    bool   smooth;         // noise_type == 1
    int    seed;           // 1..1000
    float  offset;         // trunc(deg) / 36
    float  thickness;      // 1..100 (scaled by downsample)
    float  scale;          // downsample_x.num / den
    int    mode;           // OLMDB_MODE_*
} OLMDBParams;

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
