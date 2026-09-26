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
    #include <windows.h>
    #include <stdio.h>
    #include <string.h>
    #include <stdlib.h>
#endif

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "A.h"
#include "AE_EffectUI.h"
#include "AEGP_Utils.h"
#include "AEGP_SuiteHandler.h"
#include "Mangler_Strings.h"
#include "String_Utils.h"
#include "AE_AdvEffectSuites.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"

typedef struct
{
    A_char marker_text[AEGP_MAX_MARKER_NAME_SIZE];
    AEGP_ItemH compH;
    AEGP_StreamType stream_type; // Selected Stream
} ManglerOptions;

// This entry point is exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
