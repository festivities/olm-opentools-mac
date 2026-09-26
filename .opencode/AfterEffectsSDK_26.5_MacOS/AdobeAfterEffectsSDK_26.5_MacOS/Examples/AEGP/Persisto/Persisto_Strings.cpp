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

#include "Persisto.h"

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Menu_Item,
    "Persisto!",
    StrID_Section_Key,
    "Persisto",
    StrID_Value_Key_1,
    "Fuzziness",
    StrID_Value_Key_2,
    "Cliche Du Jour",
    StrID_DefaultString,
    "Default",
    StrID_NewValueAdded,
    "New value added!",
    StrID_ValueExisted,
    "Value already existed",
    StrID_DifferentValueSet,
    "Different value found than expected!",
    StrID_Error,
    "Problems encountered while registering menu command."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
