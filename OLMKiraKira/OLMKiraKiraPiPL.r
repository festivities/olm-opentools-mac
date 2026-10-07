#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "OLM Kira Kira" },
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
		AE_Effect_Version { 1671168 },
		AE_Effect_Info_Flags { 0 },
		/* Binary out_flags are 0x02008040. The port adds
		 * SEND_UPDATE_PARAMS_UI so Use Ramp can disable the paired control.
		 * 0x06008040 = USE_OUTPUT_EXTENT | CUSTOM_UI | DEEP_COLOR_AWARE |
		 * SEND_UPDATE_PARAMS_UI. */
		AE_Effect_Global_OutFlags { 0x06008040 },
		AE_Effect_Global_OutFlags_2 { 0x08001400 },
		AE_Effect_Match_Name { "OLM OLM Kira Kira" },
		AE_Reserved_Info { 0 }
	}
};
