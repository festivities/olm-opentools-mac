#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AEFX_SuiteHelper.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"

#define OLMDG_ABOUT "DistanceGradation v0.82\rGenerate color gradation using distance transform."

#define OLMDG_MAJOR_VERSION 0
#define OLMDG_MINOR_VERSION 8
#define OLMDG_BUG_VERSION   2

// Parameter positions == uu.id.
enum {
    OLMDG_INPUT = 0,
    OLMDG_INVERT,         // checkbox
    OLMDG_IN_OUT,         // popup Inside|Outside|Both (binary default 0)
    OLMDG_INSIDE_THR,     // slider 0..1000
    OLMDG_OUTSIDE_THR,    // slider 0..1000
    OLMDG_RENDER_MODE,    // popup RGB|Layer
    OLMDG_USE_BG,         // checkbox
    OLMDG_GRAD_COLOR,     // color
    OLMDG_BG_COLOR,       // color
    OLMDG_INTERP,         // popup Constant|Linear|Sphere|Power
    OLMDG_POWER,          // float 0.01..5
    OLMDG_BLUR_MODE,      // popup No Blur|Blur No Scale|Blur
    OLMDG_BLUR_SIZE,      // slider 0..4096
    OLMDG_NUM_PARAMS
};

enum { OLMDG_INSIDE = 1, OLMDG_OUTSIDE = 2, OLMDG_BOTH = 3 };
enum { OLMDG_RENDER_RGB = 1, OLMDG_RENDER_LAYER = 2 };
enum { OLMDG_CONSTANT = 1, OLMDG_LINEAR = 2, OLMDG_SPHERE = 3, OLMDG_POWER_MODE = 4 };
enum { OLMDG_NO_BLUR = 1, OLMDG_BLUR_NO_SCALE = 2, OLMDG_BLUR = 3 };

struct OLMDGParams {
    bool invert;
    A_long in_out;
    A_long inside_thr;
    A_long outside_thr;
    A_long render_mode;
    bool use_bg;
    PF_PixelFloat grad_color;
    PF_PixelFloat bg_color;
    A_long interp;
    float power;
    A_long blur_mode;
    A_long blur_size;
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
