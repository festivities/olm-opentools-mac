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

#include "CCU.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Custom Comp UI",
    StrID_Description,
    "Manages a custom Comp (and Layer) window UI.\rCopyright 1994-2023\rAdobe Inc.",
    StrID_X_Slider_Name,
    "Horizontal Radius",
    StrID_Y_Slider_Name,
    "Vertical Radius",
    StrID_Center_Name,
    "Center",
    StrID_Err_LoadSuite,
    "Error loading suite.",
    StrID_Err_FreeSuite,
    "Error releasing suite."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}