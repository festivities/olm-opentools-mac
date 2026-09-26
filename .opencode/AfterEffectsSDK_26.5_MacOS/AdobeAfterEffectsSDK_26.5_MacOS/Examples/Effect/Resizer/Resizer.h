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

/*
    Resizer.h
*/

#pragma once

#ifndef RESIZER_H
    #define RESIZER_H

    #define PF_DEEP_COLOR_AWARE                                                                                        \
        1 // make sure we get 16bpc pixels;                                                                          \
            // AE_Effect.h checks for this.
    #include "AEConfig.h"
    #include "entry.h"
    #ifdef AE_OS_WIN
        #include "string.h"
    #endif
    #include "AE_Effect.h"
    #include "AE_EffectCB.h"
    #include "AE_Macros.h"
    #include "Param_Utils.h"
    #include "AE_EffectCBSuites.h"
    #include "String_Utils.h"
    #include "AE_GeneralPlug.h"
    #include "AEGP_SuiteHandler.h"

    #include "Resizer_Strings.h"

    #ifdef AE_OS_WIN
        #include <Windows.h>
    #endif

/* Versioning information */

    #define MAJOR_VERSION 2
    #define MINOR_VERSION 4
    #define BUG_VERSION 0
    #define STAGE_VERSION PF_Stage_DEVELOP
    #define BUILD_VERSION 1

/* Parameter defaults */

    #define RESIZE_AMOUNT_MIN 0
    #define RESIZE_AMOUNT_MAX 100
    #define RESIZE_AMOUNT_DFLT 50

enum
{
    RESIZE_INPUT = 0,
    RESIZE_AMOUNT,
    RESIZE_COLOR,
    RESIZE_DOWNSAMPLE,
    RESIZE_USE_3D,
    RESIZE_NUM_PARAMS
};

enum
{
    AMOUNT_DISK_ID = 1,
    COLOR_DISK_ID,
    DOWNSAMPLE_DISK_ID,
    THREED_DISK_ID
};

extern "C"
{

    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif // RESIZER_H