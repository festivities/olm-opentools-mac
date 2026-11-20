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

#define OLMUV_NAME       "OLM UV Map"
#define OLMUV_CATEGORY   "OLM Plug-ins"
#define OLMUV_MATCH_NAME "OLM UV Map"
#define OLMUV_DESC       "Move pixels around in an image"

#define OLMUV_MAJOR_VERSION 1
#define OLMUV_MINOR_VERSION 1
#define OLMUV_BUG_VERSION   1

// Positions equal uu.id (binary-verified: 1, 2, 3).
enum {
    OLMUV_INPUT = 0,
    OLMUV_TEXTURE = 1,
    OLMUV_INTERP = 2,
    OLMUV_SMOOTHNESS = 3,
    OLMUV_NUM_PARAMS = 4
};

// Interpolation Method popup values (1-based, binary-verified).
enum {
    OLMUV_NEAREST = 1,
    OLMUV_BILINEAR = 2,
    OLMUV_TRILINEAR = 3
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
