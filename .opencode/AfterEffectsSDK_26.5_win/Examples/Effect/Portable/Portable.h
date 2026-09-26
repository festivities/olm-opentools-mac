/*******************************************************************/
/*                                                                 */
/* Copyright 2007 Adobe                                            */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  Adobe permits you to use, modify, and distribute this  */
/* file in accordance with the terms of the Adobe license          */
/* agreement accompanying it.                                      */
/*                                                                 */
/*******************************************************************/

#ifndef PORTABLE_TABLE_H
#define PORTABLE_TABLE_H

#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "A.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "Portable_Strings.h"
#include "String_Utils.h"
#include "AEFX_SuiteHelper.h"
#include "AEGP_SuiteHandler.h"

//  BOTH suite handlers?! Yes.

//  Normally, a plug-in should rely on one or the other of
//  our suite handling utility models. Since this sample is
//  about reacting to different hosts, we'll use both.

#define MAJOR_VERSION 3
#define MINOR_VERSION 3
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

enum
{
    PORTABLE_INPUT = 0, // default input layer
    PORTABLE_SLIDER,
    PORTABLE_NUM_PARAMS
};

enum
{
    PORTABLE_DISK_ID = 1
};

typedef struct
{
    A_FpShort slider_value;
} PortableRenderInfo;

#define PORTABLE_MIN 0.0
#define PORTABLE_MAX 200.0
#define PORTABLE_BIG_MAX 200.0
#define PORTABLE_DFLT 10.0
#define SLIDER_PRECISION 1
#define DISPLAY_FLAGS PF_ValueDisplayFlag_PERCENT

extern "C"
{

    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}
#endif // PORTABLE_TABLE_H