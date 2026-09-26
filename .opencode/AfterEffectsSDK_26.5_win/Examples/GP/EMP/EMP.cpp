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

#include "EMP.h"

#ifdef AE_OS_WIN
BOOL APIENTRY LibMain(HANDLE hInstance, DWORD fdwReason, LPVOID lpReserved)
{
    printf("EMP.aex Initialization entry point.");
    return TRUE;
}
#endif

static PF_Err MyBlit(
    void* hook_refconPV,
    const AE_PixBuffer* pix_bufP0,
    const AE_ViewCoordinates* viewP,
    AE_BlitReceipt receipt,
    AE_BlitCompleteFunc complete_func0,
    AE_BlitInFlags in_flags,
    AE_BlitOutFlags* out_flags)
{
    return PF_Err_NONE;
}

static void MyDeath(void* hook_refconPV)
{
    // free anything you allocated.
}

static void MyVersion(void* hook_refconPV, A_u_long* versionPV)
{
    *versionPV = 1;
}

DllExport PF_Err EntryPointFunc(
    A_long major_version, A_long minor_version, AE_FileSpecH file_specH, AE_FileSpecH res_specH, AE_Hooks* hooksP)
{
    PF_Err err = PF_Err_NONE;
    hooksP->blit_hook_func = MyBlit;
    hooksP->death_hook_func = MyDeath;
    hooksP->version_hook_func = MyVersion;
    return err;
}
