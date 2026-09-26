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

#include "Commando.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Commando!",
    StrID_Description,
    "Commando, Copyright 2007 Adobe Systems Incorporated.\rExtremely simple command hooking plug-in.",
    StrID_GenericError,
    "Error enabling Commando."

};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
