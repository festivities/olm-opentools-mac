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

#define OLMBLUR_NAME  "OLM Blur"
#define OLMBLUR_ABOUT "OLM Blur v1.2.1\rBlur the source while keeping the selected divisions.\nCopyright 2014 OLM Digital, Inc."

#define OLMBLUR_MAJOR_VERSION 1
#define OLMBLUR_MINOR_VERSION 2
#define OLMBLUR_BUG_VERSION   1

// Parameter positions (checkout / AEGP stream index).
enum {
    OLMBLUR_INPUT = 0,
    OLMBLUR_AMOUNT,      // uu.id 5
    OLMBLUR_SMOOTHNESS,  // uu.id 6 (always hidden; Legacy only)
    OLMBLUR_REPEAT,      // uu.id 3
    OLMBLUR_BIAS,        // uu.id 4
    OLMBLUR_LEGACY,      // uu.id 7
    OLMBLUR_NUM_PARAMS
};

enum { OLMBLUR_BIAS_VERTICAL = 1, OLMBLUR_BIAS_HORIZONTAL = 2 };

struct OLMBlurParams {
    float amount;      // Blur Amount (double param -> float)
    float smoothness;  // integer part of the fixed slider
    A_long repeat;
    A_long bias;
    bool legacy;
};

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
