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

#include "AEConfig.h"
#include "entry.h"

#ifdef AE_OS_WIN
    #include <windows.h>
    #include <stdio.h>
    #include <string.h>
#elif defined AE_OS_MAC
    #include <wchar.h>
#endif

#include "AE_GeneralPlug.h"
#include "AE_Effect.h"
#include "A.h"
#include "AE_EffectUI.h"
#include "SPSuites.h"
#include "AE_AdvEffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"

#ifdef AE_OS_WIN
    #define PROXY_FOOTAGE_PATH L"C:\\proxy.jpg"
    #define LAYERED_PATH L"C:\\noel_clown_nose.psd"
#elif defined AE_OS_MAC
    #define PROXY_FOOTAGE_PATH L"proxy.jpg"
    #define LAYERED_PATH L"noel_clown_nose.psd"
#endif

#define ERROR_MISSING_FOOTAGE                                                                                          \
    "Footage not found! Make sure you've copied proxy.jpg from the ProjDumper folder and noel_clown_nose.psd from the Projector folder to the root of your primary hard disk."
#define DUMP_SUCCEEDED_WIN "Project dumped to C:\\Windows\\Temp\\"
#define DUMP_SUCCEEDED_MAC "Project dumped to root of primary hard drive."
#define DUMP_FAILED "Project dump failed."

// This entry point is exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
