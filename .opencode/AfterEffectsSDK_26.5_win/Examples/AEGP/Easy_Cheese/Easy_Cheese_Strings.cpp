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

#include "Easy_Cheese.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Easy Cheese",
    StrID_Description,
    "Keyframer plug-in.Copyright 1994-2023 Adobe Inc.",
    StrID_MarkerText,
    "Easy Cheese was here.",
    StrID_URL,
    "http://www.adobe.com",
    StrID_Chapter,
    "chapter %d",
    StrID_SuiteError,
    "Error acquiring suite."

};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
