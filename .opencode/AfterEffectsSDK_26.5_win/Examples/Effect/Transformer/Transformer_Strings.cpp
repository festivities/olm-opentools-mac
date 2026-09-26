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

#include "Transformer.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Transformer",
    StrID_Description,
    "Demonstrate image processing callbacks.\nCopyright 2007-2023 Adobe Inc.",
    StrID_ColorBlendSlider_Name,
    "Color Blend Ratio",
    StrID_Color_Param_Name,
    "Color",
    StrID_Topic_Name,
    "Layer Controls",
    StrID_LayerBlendSlider_Name,
    "Layer Opacity",
    StrID_Layer_Param_Name,
    "Layer Blend Ratio",
    StrID_Err_LoadSuite,
    "Error loading suite.",
    StrID_Err_FreeSuite,
    "Error releasing suite.",
};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
