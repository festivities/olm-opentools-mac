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

#pragma once

#ifndef EMP_H
    #define EMP_H

    #include "AEConfig.h"
    #include "A.h"
    #include "AE_Effect.h"
    #include "AE_EffectCB.h"
    #include "AE_Macros.h"
    #include "AE_Hook.h"
    #include "entry.h"

    #ifdef AE_OS_WIN
        #include <stdio.h>
        #include <windows.h>
    #endif

    #define MAJOR_VERSION 1
    #define MINOR_VERSION 0
    #define BUG_VERSION 0
    #define STAGE_VERSION PF_Stage_DEVELOP
    #define BUILD_VERSION 0

    #define NAME "EMP"
    #define DESCRIPTION "External Monitor Preview"

    #include "AE_Hook.h"

extern "C"
{
    DllExport PF_Err EntryPointFunc(
        A_long major_version,    /* >> */
        A_long minor_version,    /* >> */
        AE_FileSpecH file_specH, /* >> */
        AE_FileSpecH res_specH,  /* >> */
        AE_Hooks* hooksP);       /* <> */
}
    #ifdef AE_OS_WIN
BOOL APIENTRY LibMain(HANDLE hInstance, DWORD fdwReason, LPVOID lpReserved);
    #endif

#endif //EMP_H