/*******************************************************************/
/*                                                                 */
/* Copyright 2025 Adobe                                            */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  Adobe permits you to use, modify, and distribute this  */
/* file in accordance with the terms of the Adobe license          */
/* agreement accompanying it.                                      */
/*                                                                 */
/*******************************************************************/

//Mangler_Strings.cpp

#include "Mangler.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Mangler!",
    StrID_Description,
    "Keyframer plug-in.Copyright 1994-2023 Adobe Inc.",
    StrID_SuiteError,
    "Error acquiring suite.",
    StrID_InitError,
    "Error initializing dialog.",
    StrID_AddCommandError,
    "Error adding command.",
    StrID_UndoError,
    "Error starting undo group.",
    StrID_URL,
    "http://www.adobe.com",
    StrID_Tagline,
    "Tagline:",
    StrID_MessedUpLayerName,
    "Mangled layer!!!",
    StrID_MessedUpMaskName,
    "Mangled mask!!!",
    StrID_NoPositionKeyframes,
    "Please add some position keyframes, so Mangler can exercise the keyframe suite."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
