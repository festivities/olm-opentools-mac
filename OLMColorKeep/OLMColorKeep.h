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

#define OLMCKP_NAME  "Color Keep"
#define OLMCKP_ABOUT "Color Keep v1.01\rKeep the selected color from the source.\rCopyright 2010 OLM Digital, Inc."

#define OLMCKP_MAJOR_VERSION 1
#define OLMCKP_MINOR_VERSION 0
#define OLMCKP_BUG_VERSION   1

// Positions equal uu.id here: 0 input, 1 Enabled Color Num, 2..101 Color.
enum {
    OLMCKP_INPUT = 0,
    OLMCKP_COUNT = 1,
    OLMCKP_COLOR_BASE = 2
};

constexpr int OLMCKP_MAX_COLORS = 100;
constexpr int OLMCKP_NUM_PARAMS = OLMCKP_COLOR_BASE + OLMCKP_MAX_COLORS;  // 102

struct OLMCKPParams {
    A_long count;
    PF_PixelFloat colors[OLMCKP_MAX_COLORS];
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
