#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "OLM Toon Dilate" },
		Category { "OLM Plug-ins" },
#ifdef AE_OS_WIN
	#if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
	#elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
	#endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { 13, 29 },
		AE_Effect_Version { 559104 },
		AE_Effect_Info_Flags { 0 },
		/* 0x02000044 = NON_PARAM_VARY | USE_OUTPUT_EXTENT | DEEP_COLOR_AWARE. */
		AE_Effect_Global_OutFlags { 0x02000044 },
		/* 0x08021400 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE |
		 * AUTOMATIC_WIDE_TIME_INPUT | SUPPORTS_THREADED_RENDERING. */
		AE_Effect_Global_OutFlags_2 { 0x08021400 },
		AE_Effect_Match_Name { "ADBE OLMToonDilate" },
		AE_Reserved_Info { 0 }
	}
};
