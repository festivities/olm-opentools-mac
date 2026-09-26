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

// Streamie_Strings.cpp

#include "Streamie.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Streamie!",
    StrID_ChangedName,
    "Changed by Streamie!",
    StrID_CommandName,
    "To enable Streamie, select one paint-ed layer",
    StrID_Troubles,
    "Streamie: Problems encountered during stream twiddling."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
