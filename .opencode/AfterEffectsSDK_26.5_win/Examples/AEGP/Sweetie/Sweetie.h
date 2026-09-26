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
#elif defined AE_OS_MAC
    #include <AudioToolbox/AudioServices.h>
#endif

#include "AE_GeneralPlug.h"
#include "A.h"
#include "SPSuites.h"
#include "AE_Macros.h"

#include "DuckSuite.h"

// This entry point is exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
