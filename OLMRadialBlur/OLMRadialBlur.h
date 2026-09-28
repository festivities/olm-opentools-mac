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

#define OLMRB_NAME        "OLM RadialBlur"
#define OLMRB_CATEGORY    "OLM Plug-ins"
#define OLMRB_PIPL_NAME   "OLM RadialBlur"
#define OLMRB_MATCH_NAME  "OLM RadialBlur"
#define OLMRB_ABOUT       "OLM RadialBlur 1.3\rApply circular and directional blur"

#define OLMRB_MAJOR_VERSION 1
#define OLMRB_MINOR_VERSION 3
#define OLMRB_BUG_VERSION   0

// PF_ParamDef.uu.id values from the Windows binary. Parameter checkout APIs
// take the separate zero-based position enum below.
enum OLMRB_ParamId {
    OLMRB_ID_BLUR_TYPE = 1,
    OLMRB_ID_CENTER = 2,
    OLMRB_ID_OUTER_GROUP = 3,
    OLMRB_ID_OUTER_STRENGTH = 4,
    OLMRB_ID_OUTER_FADE = 5,
    OLMRB_ID_OUTER_GROUP_END = 6,
    OLMRB_ID_INNER_GROUP = 7,
    OLMRB_ID_INNER_STRENGTH = 8,
    OLMRB_ID_INNER_FADE = 9,
    OLMRB_ID_INNER_GROUP_END = 10,
    OLMRB_ID_ELLIPSE_GROUP = 11,
    OLMRB_ID_RATIO = 12,
    OLMRB_ID_ANGLE = 13,
    OLMRB_ID_ELLIPSE_GROUP_END = 14,
    OLMRB_ID_QUALITY = 15,
    OLMRB_ID_BRIGHTNESS = 16,
    OLMRB_ID_SIZE_VARIATION = 17,
    OLMRB_ID_NOISE_GROUP = 18,
    OLMRB_ID_NOISE_VARIATION = 19,
    OLMRB_ID_NOISE_TYPE = 20,
    OLMRB_ID_NOISE_LAYER = 21,
    OLMRB_ID_SEED = 22,
    OLMRB_ID_NOISE_OFFSET = 23,
    OLMRB_ID_THICKNESS = 24,
    OLMRB_ID_NOISE_GROUP_END = 25,
    OLMRB_ID_REPEAT_BORDER = 26,
    OLMRB_ID_OUTER_OFFSET_MODE = 28,
    OLMRB_ID_OUTER_OFFSET = 29,
    OLMRB_ID_INNER_OFFSET_MODE = 30,
    OLMRB_ID_INNER_OFFSET = 31
};

// params[0] is the input layer. These are parameter positions, not uu.id.
enum OLMRB_ParamPosition {
    OLMRB_INPUT = 0,
    OLMRB_BLUR_TYPE = 1,
    OLMRB_CENTER = 2,
    OLMRB_OUTER_GROUP = 3,
    OLMRB_OUTER_STRENGTH = 4,
    OLMRB_OUTER_OFFSET_MODE = 5,
    OLMRB_OUTER_OFFSET = 6,
    OLMRB_OUTER_FADE = 7,
    OLMRB_OUTER_GROUP_END = 8,
    OLMRB_INNER_GROUP = 9,
    OLMRB_INNER_STRENGTH = 10,
    OLMRB_INNER_OFFSET_MODE = 11,
    OLMRB_INNER_OFFSET = 12,
    OLMRB_INNER_FADE = 13,
    OLMRB_INNER_GROUP_END = 14,
    OLMRB_REPEAT_BORDER = 15,
    OLMRB_ELLIPSE_GROUP = 16,
    OLMRB_RATIO = 17,
    OLMRB_ANGLE = 18,
    OLMRB_ELLIPSE_GROUP_END = 19,
    OLMRB_QUALITY = 20,
    OLMRB_BRIGHTNESS = 21,
    OLMRB_SIZE_VARIATION = 22,
    OLMRB_NOISE_GROUP = 23,
    OLMRB_NOISE_VARIATION = 24,
    OLMRB_NOISE_TYPE = 25,
    OLMRB_NOISE_LAYER = 26,
    OLMRB_SEED = 27,
    OLMRB_NOISE_OFFSET = 28,
    OLMRB_THICKNESS = 29,
    OLMRB_NOISE_GROUP_END = 30,
    OLMRB_NUM_PARAMS = 31
};

enum OLMRB_PopupValue {
    OLMRB_BLUR_ZOOM = 1,
    OLMRB_BLUR_ROTATION = 2,
    OLMRB_OFFSET_ADD = 1,
    OLMRB_OFFSET_MAX = 2,
    OLMRB_OFFSET_OVERRIDE = 3,
    OLMRB_NOISE_SMOOTH = 1,
    OLMRB_NOISE_BLOCK = 2,
    OLMRB_NOISE_LAYER_MODE = 3
};

typedef struct {
    A_long blur_type;
    double center_x;
    double center_y;
    float brightness;
    float noise_variation;
    float size_variation;
    bool size_variation_active;
    A_long noise_type;
    A_long seed;
    float noise_offset_rad;
    float thickness;
    float angle_rad;
    bool repeat_border;
    float ratio;
    float quality_step_degrees;
    float scale;
    A_long outer_strength;
    A_long inner_strength;
    A_long outer_offset_mode;
    A_long inner_offset_mode;
    A_long outer_offset;
    A_long inner_offset;
    A_long outer_fade;
    A_long inner_fade;
    PF_LayerDef noise_layer;
    bool has_noise_layer;
} OLMRBParams;

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
