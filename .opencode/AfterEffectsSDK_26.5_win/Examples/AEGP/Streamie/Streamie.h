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
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"
#include "String_Utils.h"
#include "Streamie_Strings.h"
#include "AEGP_Utils.h"

// This entry point is exported through the PiPL (.r file)
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;
