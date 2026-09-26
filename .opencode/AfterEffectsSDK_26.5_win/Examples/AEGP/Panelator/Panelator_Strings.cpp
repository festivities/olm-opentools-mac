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

#include "Panelator.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Panelator!",
    StrID_Description,
    "Panelator, Copyright 2007-2023 Adobe Inc.\rExtremely simple command hooking plug-in.",
    StrID_GenericError,
    "Error enabling Panelator."

};

A_char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
