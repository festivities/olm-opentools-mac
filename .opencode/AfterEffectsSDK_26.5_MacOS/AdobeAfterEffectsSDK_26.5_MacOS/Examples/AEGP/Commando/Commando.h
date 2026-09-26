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
#endif

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "String_Utils.h"
#include "Commando_Strings.h"

#define AEGP_MAX_STREAM_DIM 4

typedef enum
{
    StrID_NONE,
    StrID_Name,
    StrID_Description,
    StrID_GenericError,
    StrID_NUMTYPES
} StrIDType;

extern "C"
{
    DllExport A_Err EntryPointFunc(
        struct SPBasicSuite* pica_basicP,
        A_long major_versionL,
        A_long minor_versionL,
        const A_char* file_pathZ,
        const A_char* res_pathZ,
        AEGP_PluginID aegp_plugin_id,
        void* global_refconPV);
}
