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

#include "QueueBert.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "QueueBert",
    StrID_Pronounce,
    "That's pronounced cue-BARE!",
    StrID_Troubles,
    "QueueBert: Problems encountered during add."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
