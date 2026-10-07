#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		/* Binary PiPL: Name is "Color Keep" (no OLM prefix); Match Name has it. */
		Name { "Color Keep" },
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
		AE_Effect_Version { 526336 },
		AE_Effect_Info_Flags { 0 },
		/* Binary raw out_flags are 0x02000040 (USE_OUTPUT_EXTENT |
		 * DEEP_COLOR_AWARE); the port adds SEND_UPDATE_PARAMS_UI (bit 26). */
		AE_Effect_Global_OutFlags { 0x06000040 },
		/* 0x08001400 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE |
		 * SUPPORTS_THREADED_RENDERING (binary-exact). */
		AE_Effect_Global_OutFlags_2 { 0x08001400 },
		AE_Effect_Match_Name { "OLM Color Keep" },
		AE_Reserved_Info { 0 }
	}
};
