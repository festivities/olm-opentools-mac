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

#define OLMCK_NAME        "OLM Color Key"
#define OLMCK_CATEGORY    "OLM Plug-ins"
#define OLMCK_PIPL_NAME   "OLM Color Key"
#define OLMCK_MATCH_NAME  "OLM Color Key"
#define OLMCK_ABOUT       "OLM Color Key 2.3.3\rKey or Keep the selected color from the source.\rCopyright 2014 OLM Digital, Inc."

#define OLMCK_MAJOR_VERSION 2
#define OLMCK_MINOR_VERSION 3
#define OLMCK_BUG_VERSION   3

// PF_ParamDef.uu.id values from the Windows binary. Parameter checkout /
// PF_UpdateParamUI / AEGP stream APIs take the separate zero-based position
// enum below; uu.id is identity only.
enum OLMCK_ParamId {
    OLMCK_ID_COLOR_KEEP = 1,
    OLMCK_ID_THRESHOLD = 2,
    OLMCK_ID_THRESHOLD_GROUP = 3,
    OLMCK_ID_PREMULTIPLIED = 4,
    OLMCK_ID_COLOR_SPACE = 5,
    OLMCK_ID_FORCE_PRECISION = 522,
    OLMCK_ID_PER_COLOR = 6,
    OLMCK_ID_PER_COMPONENT = 7,
    OLMCK_ID_THRESHOLD_R = 8,
    OLMCK_ID_THRESHOLD_G = 9,
    OLMCK_ID_THRESHOLD_B = 10,
    OLMCK_ID_THRESHOLD_GROUP_END = 11,
    OLMCK_ID_THIN_GROUP = 12,
    OLMCK_ID_THIN_AMOUNT = 13,
    OLMCK_ID_THIN_DISTANCE = 14,
    OLMCK_ID_THIN_GROUP_END = 20,
    OLMCK_ID_BLUR_GROUP = 16,
    OLMCK_ID_BLUR_AMOUNT = 17,
    OLMCK_ID_BLUR_DISTANCE = 18,
    OLMCK_ID_BLUR_DIRECTION = 19,
    OLMCK_ID_BLUR_GROUP_END = 16, // duplicate id in the binary; positions differ
    OLMCK_ID_COLOR_COUNT = 21,
    OLMCK_ID_ENABLE_REPLACE = 523
};

// Slot n (0..OLMCK_MAX_COLORS-1) uu.id values, binary-exact for 2.3.3. The binary
// (setup 0x180001000, checkout 0x18000A8E0, UI 0x180001D90) uses two blocks:
// slots 0..99 and 100..199. Positions are sequential (24 + 8n) in both blocks.
constexpr int OLMCK_SlotIdUseColor(int n) { return n < 100 ? 524 + 3 * n : 1024 + 3 * n; }
constexpr int OLMCK_SlotIdUseReplace(int n) { return n < 100 ? 525 + 3 * n : 1025 + 3 * n; }
constexpr int OLMCK_SlotIdReplaceColor(int n) { return n < 100 ? 526 + 3 * n : 1026 + 3 * n; }
constexpr int OLMCK_SlotIdColor(int n) { return n < 100 ? 22 + 5 * n : 324 + 5 * n; }
constexpr int OLMCK_SlotIdThreshold(int n) { return n < 100 ? 23 + 5 * n : 325 + 5 * n; }
constexpr int OLMCK_SlotIdThresholdR(int n) { return n < 100 ? 24 + 5 * n : 326 + 5 * n; }
constexpr int OLMCK_SlotIdThresholdG(int n) { return n < 100 ? 25 + 5 * n : 327 + 5 * n; }
constexpr int OLMCK_SlotIdThresholdB(int n) { return n < 100 ? 26 + 5 * n : 328 + 5 * n; }

// params[0] is the input layer. These are parameter positions, not uu.id.
enum OLMCK_ParamPosition {
    OLMCK_INPUT = 0,
    OLMCK_COLOR_KEEP = 1,
    OLMCK_THRESHOLD = 2,
    OLMCK_THRESHOLD_GROUP = 3,
    OLMCK_PREMULTIPLIED = 4,
    OLMCK_COLOR_SPACE = 5,
    OLMCK_FORCE_PRECISION = 6,
    OLMCK_PER_COLOR = 7,
    OLMCK_PER_COMPONENT = 8,
    OLMCK_THRESHOLD_R = 9,
    OLMCK_THRESHOLD_G = 10,
    OLMCK_THRESHOLD_B = 11,
    OLMCK_THRESHOLD_GROUP_END = 12,
    OLMCK_THIN_GROUP = 13,
    OLMCK_THIN_AMOUNT = 14,
    OLMCK_THIN_DISTANCE = 15,
    OLMCK_THIN_GROUP_END = 16,
    OLMCK_BLUR_GROUP = 17,
    OLMCK_BLUR_AMOUNT = 18,
    OLMCK_BLUR_DISTANCE = 19,
    OLMCK_BLUR_DIRECTION = 20,
    OLMCK_BLUR_GROUP_END = 21,
    OLMCK_COLOR_COUNT = 22,
    OLMCK_ENABLE_REPLACE = 23,
    // Slot n (0..199) occupies 8 consecutive positions starting at 24 + 8n:
    OLMCK_SLOT_BASE = 24
};

constexpr int OLMCK_MAX_COLORS = 200;
constexpr int OLMCK_NUM_PARAMS = OLMCK_SLOT_BASE + OLMCK_MAX_COLORS * 8; // 1624

enum OLMCK_PopupValue {
    OLMCK_SPACE_RGB = 1,
    OLMCK_SPACE_HSV = 2,
    OLMCK_SPACE_LAB76 = 3,
    OLMCK_SPACE_LAB94 = 4,
    OLMCK_SPACE_YUV = 5,
    OLMCK_SPACE_YCRCB = 6,
    OLMCK_PREC_FULL = 1,
    OLMCK_PREC_16 = 2,
    OLMCK_PREC_8 = 3,
    OLMCK_DIST_BOX = 1,
    OLMCK_DIST_APPROX = 2,
    OLMCK_DIST_EUCLIDEAN = 3,
    OLMCK_DIR_INSIDE = 1,
    OLMCK_DIR_AROUND = 2,
    OLMCK_DIR_OUTSIDE = 3
};

typedef struct {
    bool keep;
    bool premultiplied;
    A_long thin_amount;
    A_long thin_distance;
    A_long color_space;
    bool per_color;
    bool per_component;
    A_long count;
    A_long force_precision;
    float blur_amount;
    A_long blur_distance;
    A_long blur_direction;
    bool enable_replace;
    float global_threshold;
    float threshold_rgb[3];
    PF_PixelFloat key_colors[OLMCK_MAX_COLORS];
    PF_PixelFloat replace_colors[OLMCK_MAX_COLORS];
    float thresholds[OLMCK_MAX_COLORS];
    float threshold_rgbs[OLMCK_MAX_COLORS][3];
    bool use_color[OLMCK_MAX_COLORS];
    bool use_replace[OLMCK_MAX_COLORS];
    A_long bitdepth;
    float epsilon;
} OLMCKParams;

extern "C" {
DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
                            PF_ParamDef *params[], PF_LayerDef *output, void *extra);
}
