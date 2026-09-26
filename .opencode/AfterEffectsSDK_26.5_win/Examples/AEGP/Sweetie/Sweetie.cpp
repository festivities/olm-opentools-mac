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

/*  Sweetie's whole purpose is to provide a PICA suite for use by 
    other plug-ins (specifically, the Shifter effect sample). 
*/

#include "Sweetie.h"

// The incredibly handy DuckSuite! Shows how to export a
// PICA Suite from an AEGP.

static SPAPI A_Err Quack(A_u_short timesSu)
{
    for (A_u_short iSu = 0; iSu < timesSu; iSu++)
    {
#ifdef AE_OS_WIN
        MessageBox(0, "Quack!", "AEGP-provided PICA Suite!", MB_OK);
#else
        AudioServicesPlayAlertSound(kUserPreferredAlert);
#endif
    }
    return A_Err_NONE;
}

static DuckSuite1 S_duck_suite = {Quack};

A_Err EntryPointFunc(
    struct SPBasicSuite* pica_basicP,  /* >> */
    A_long major_versionL,             /* >> */
    A_long minor_versionL,             /* >> */
    AEGP_PluginID aegp_plugin_id,      /* >> */
    AEGP_GlobalRefcon* global_refconP) /* << */
{
    SPSuiteRef my_ref = 0;
    A_Err err = A_Err_NONE;
    SPSuitesSuite* suites_suiteP = 0;

    try
    {
        ERR(pica_basicP->AcquireSuite(kSPSuitesSuite, kSPSuitesSuiteVersion, (const void**)&suites_suiteP));

        if (suites_suiteP)
        {
            ERR(suites_suiteP->AddSuite(
                kSPRuntimeSuiteList, 0, kDuckSuite1, kDuckSuiteVersion1, 1, &S_duck_suite, &my_ref));
        }
        ERR(pica_basicP->ReleaseSuite(kSPSuitesSuite, kSPSuitesSuiteVersion));
    }
    catch (A_Err& thrown_err)
    {
        err = thrown_err;
    }
    return err;
}