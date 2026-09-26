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

// Text_Twiddler_Strings.h

#pragma once

typedef enum
{
    StrID_NONE,
    StrID_Name,
    StrID_Selection,
    StrID_CommandName,
    StrID_Troubles,
    StrID_NUMTYPES
} StrIDType;

char* GetStringPtr(int strNum);