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

#include "Resizer.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Resizer",
    StrID_Description,
    "Demonstrate Output Buffer Resizing.\rCopyright 1994-2023 Adobe Inc.",
    StrID_Color_Param_Name,
    "Resized Area Color",
    StrID_Checkbox_Param_Name,
    "Use Downsample Factors",
    StrID_Checkbox_Description,
    "Correct at all resolutions",
    StrID_DependString1,
    "All Dependencies requested.",
    StrID_DependString2,
    "Missing Dependencies requested.",
    StrID_Err_LoadSuite,
    "Error loading suite.",
    StrID_Err_FreeSuite,
    "Error releasing suite.",
    StrID_3D_Param_Name,
    "Use lights and cameras",
    StrID_3D_Param_Description,
    "(new in 5.0!)",
    StrID_Exception,
    "Caught an exception! Uh-oh, missing suite..."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
