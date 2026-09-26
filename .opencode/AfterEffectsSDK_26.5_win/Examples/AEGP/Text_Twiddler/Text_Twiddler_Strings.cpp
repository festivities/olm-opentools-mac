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

// Text_Twiddler_Strings.cpp

#include "Text_Twiddler.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Text Twiddler",
    StrID_Selection,
    "Text Twiddler (select one text layer)",
    StrID_CommandName,
    "Invoke Text Twiddler",
    StrID_Troubles,
    "Text_Twiddler: Problems encountered during twiddling."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
