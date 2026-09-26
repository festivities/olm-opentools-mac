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

#include "Grabba.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Grabba",
    StrID_Description,
    "Frame grabbing plug-in.Copyright 1994-2023 Adobe Inc.",
    StrID_IdleCount,
    "Grabba : IdleHook called %d times.",
    StrID_SuiteError,
    "Error acquiring suite."

};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
