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

#define OLMTD_NAME       "OLM Toon Dilate"
#define OLMTD_CATEGORY   "OLM Plug-ins"
#define OLMTD_MATCH_NAME "ADBE OLMToonDilate"
#define OLMTD_ABOUT      "OLM Toon Dilate 1.1.1\rToon Dilate Effect"

#define OLMTD_MAJOR_VERSION 1
#define OLMTD_MINOR_VERSION 1
#define OLMTD_BUG_VERSION   1

enum {
    OLMTD_INPUT = 0,
    OLMTD_RADIUS = 1,
    OLMTD_NUM_PARAMS = 2
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
