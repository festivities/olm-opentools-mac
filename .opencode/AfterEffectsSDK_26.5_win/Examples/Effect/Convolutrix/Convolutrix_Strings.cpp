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

/* Convolutrix_Strings.cpp */

#include "Convolutrix.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Convolutrix",
    StrID_Description,
    "Demonstrate our image processing callbacks.\rCopyright 2007-2023 Adobe Inc.",
    StrID_Amount_Param_Name,
    "Convolve",
    StrID_Color_Param_Name,
    "Blend color",
    StrID_Blend_Amount_Param_Name,
    "Blend percentage",
    StrID_TopicName,
    "Blend Controls",
    StrID_DependString1,
    "All Dependencies requested.",
    StrID_DependString2,
    "Missing Dependencies requested.",
    StrID_Err_LoadSuite,
    "Error loading suite.",
    StrID_Err_FreeSuite,
    "Error releasing suite.",
};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
