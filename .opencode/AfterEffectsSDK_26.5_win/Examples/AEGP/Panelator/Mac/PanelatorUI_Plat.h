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

#include "PanelatorUI.h"

#include <Carbon/Carbon.h>

class PanelatorUI_Plat : public PanelatorUI
{

public:
    explicit PanelatorUI_Plat(
        SPBasicSuite* spbP,
        AEGP_PanelH panelH,
        AEGP_PlatformViewRef platformWindowRef,
        AEGP_PanelFunctions1* outFunctionTableP);

    virtual void InvalidateAll();

private:
};
