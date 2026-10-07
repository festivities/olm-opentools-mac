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
#include "Param_Utils.h"
#include "Smart_Utils.h"

#define OLMKK_NAME       "OLM Kira Kira"
#define OLMKK_MATCH_NAME "OLM OLM Kira Kira"
#define OLMKK_ABOUT      "OLM Kira Kira 3.3\rParameterized Kira Kira Effect"

#define OLMKK_MAJOR_VERSION 3
#define OLMKK_MINOR_VERSION 3
#define OLMKK_BUG_VERSION   0

// Positions. uu.id is not the position.
enum {
    OLMKK_INPUT = 0,
    OLMKK_CHANNEL = 1,
    OLMKK_BLUR_MODE = 2,
    OLMKK_MERGE = 3,
    OLMKK_APPROX = 4,
    OLMKK_GAIN = 5,
    OLMKK_STRENGTH = 6,
    OLMKK_FADE = 7,
    OLMKK_GLOW_OPACITY = 8,
    OLMKK_SRC_OPACITY = 9,
    OLMKK_V_LEN = 10,
    OLMKK_V_COLOR = 11,
    OLMKK_V_GROUP = 12,
    OLMKK_V_USE_RAMP = 13,
    OLMKK_V_RAMP = 14,
    OLMKK_V_GROUP_END = 15,
    OLMKK_H_LEN = 16,
    OLMKK_H_COLOR = 17,
    OLMKK_H_GROUP = 18,
    OLMKK_H_USE_RAMP = 19,
    OLMKK_H_RAMP = 20,
    OLMKK_H_GROUP_END = 21,
    OLMKK_D_LEN = 22,
    OLMKK_D_COLOR = 23,
    OLMKK_D_GROUP = 24,
    OLMKK_D_USE_RAMP = 25,
    OLMKK_D_RAMP = 26,
    OLMKK_D_GROUP_END = 27,
    OLMKK_D2_LEN = 28,
    OLMKK_D2_COLOR = 29,
    OLMKK_D2_GROUP = 30,
    OLMKK_D2_USE_RAMP = 31,
    OLMKK_D2_RAMP = 32,
    OLMKK_D2_GROUP_END = 33,
    OLMKK_HL_RADIUS = 34,
    OLMKK_HL_COLOR = 35,
    OLMKK_HL_GROUP = 36,
    OLMKK_HL_USE_RAMP = 37,
    OLMKK_HL_RAMP = 38,
    OLMKK_HL_GROUP_END = 39,
    OLMKK_ROTATION = 40,
    OLMKK_NUM_PARAMS = 41
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
