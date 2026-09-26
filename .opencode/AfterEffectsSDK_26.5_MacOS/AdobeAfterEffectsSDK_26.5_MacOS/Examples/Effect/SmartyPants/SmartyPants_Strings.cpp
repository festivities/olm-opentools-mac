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

#include "SmartyPants.h"

typedef struct
{
    A_u_long index;
    A_char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "SmartyPants",
    StrID_Description,
    "Demonstrate new SmartFX render messaging.\nCopyright 2007-2023 Adobe Inc.",
    StrID_ChannelParam_Name,
    "Channel",
    StrID_Channels,
    "RGB|Red|Green|Blue|(-|HLS|Hue|Lightness|Saturation|(-|YIQ|Luminance|In Phase Chrominance|Quadrature Chrominance|(-|Alpha",
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

A_char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
