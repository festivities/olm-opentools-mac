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

// Custom_ECW_UI_Strings.cpp

#include "Custom_ECW_UI.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Custom_ECW_UI",
    StrID_Description,
    "Example using CustomUI in the effect control window.\rCopyright 2007-2023 Adobe Inc.",
    StrID_Color_Param_Name,
    "Fill Color",
    StrID_Slider_Param_Name,
    "Fancy Slider",
    StrID_Err_LoadSuite,
    "Error loading suite.",
    StrID_Err_FreeSuite,
    "Error releasing suite.",
    StrID_Frank,
    "Frank"};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}