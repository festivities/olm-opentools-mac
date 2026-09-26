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

#include "Supervisor.h"

typedef struct
{
    unsigned long index;
    char str[256];
} TableString;

#define kMenuItemSeparator "|"

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "Supervisor",
    StrID_Description,
    "Demonstrates parameter supervision. Also, dig the whizzy seperators in the 'Flavor' pop-up!\rCopyright 2007-2023\rAdobe Inc.",

    StrID_ModeName,
    "Mode",
    StrID_ModeChoices,
    "Basic" kMenuItemSeparator "Advanced",

    StrID_FlavorName,
    "Flavor",

    StrID_FlavorChoicesBasic,
    "Chocolate" kMenuItemSeparator "(-" kMenuItemSeparator "Strawberry" kMenuItemSeparator "(-" kMenuItemSeparator
    "Sherbet",

    StrID_FlavorChoicesAdvanced,
    "Exploding Snaps" kMenuItemSeparator "(-" kMenuItemSeparator "Treacle Tart" kMenuItemSeparator
    "(-" kMenuItemSeparator "Diracawl Slices (with gravy)" kMenuItemSeparator "(-" kMenuItemSeparator "Butter Beer",

    StrID_ColorName,
    "Color",

    StrID_SliderName,
    "Slider",

    StrID_CheckboxName,
    "Checkbox",
    StrID_CheckboxCaption,
    "Set slider to 50%",

    StrID_Err_LoadSuite,
    "Couldn't acquire suite.",
    StrID_Err_FreeSuite,
    "Couldn't free suite.",

    StrID_FlavorNameDisabled,
    "Flavor (disabled: Advanced)",

    StrID_GeneralError,
    "Error handling Changed Param."};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
