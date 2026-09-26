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
#ifdef AE_OS_WIN
    #define PSD_PATH L"c:\\noel_clown_nose.psd"
    #define SRC_FOOTAGE_PATH L"c:\\noel_clown_nose.psd"
    #define PROXY_FOOTAGE_PATH L"c:\\proxy.jpg"
    #define RENDER_PATH L"c:\\stinky.avi"
    #define INPUT_WHITE_PARAM 4
    #define INPUT_BLACK_PARAM 5
    #include "stdio.h"
    #include "string.h"
#elif defined AE_OS_MAC
    #define PSD_PATH L"/noel_clown_nose.psd"
    #define SRC_FOOTAGE_PATH L"/copy_to_root.jpg"
    #define PROXY_FOOTAGE_PATH L"/proxy.jpg"
    #define RENDER_PATH L"/stinky.mov"
    #define INPUT_WHITE_PARAM 3
    #define INPUT_BLACK_PARAM 4
    #include <wchar.h>
#endif
#define FOLDER_NAME L"SDK Fake Project"

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AE_Effect.h"
#include "AE_EffectUI.h"
#include "AE_EffectCB.h"
#include "AE_AdvEffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"

// This entry point is exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
