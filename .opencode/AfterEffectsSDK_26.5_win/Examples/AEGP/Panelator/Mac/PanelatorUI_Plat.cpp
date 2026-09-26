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

#include <string>
#include <vector>
#include "PT_Err.h"
#include "PanelatorUI_Plat.h"
#include <limits>

#define kClickMePressed 'CLik'
#define kMenuAboutToShow kEventMenuPopulate

PanelatorUI_Plat::PanelatorUI_Plat(
    SPBasicSuite* spbP,
    AEGP_PanelH panelH,
    AEGP_PlatformViewRef platformViewRef,
    AEGP_PanelFunctions1* outFunctionTable)
    : PanelatorUI(spbP, panelH, platformViewRef, outFunctionTable)
{

    //  shouldn't we cleanup and throw if we get an error??
}

void PanelatorUI_Plat::InvalidateAll()
{
}