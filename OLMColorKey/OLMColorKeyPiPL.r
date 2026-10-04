#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "OLM Color Key" },
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
		AE_Effect_Version { 1148928 },
		AE_Effect_Info_Flags { 0 },
		/* Binary raw out_flags are 0x02000440; the port adds
		 * PF_OutFlag_SEND_UPDATE_PARAMS_UI (bit 26) so AE sends
		 * PF_Cmd_UPDATE_PARAMS_UI for the parameter visibility logic.
		 * 0x06000440 = USE_OUTPUT_EXTENT | PIX_INDEPENDENT |
		 * DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI. */
		AE_Effect_Global_OutFlags { 0x06000440 },
		/* 0x08001400 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE |
		 * SUPPORTS_THREADED_RENDERING (binary-exact). */
		AE_Effect_Global_OutFlags_2 { 0x08001400 },
		AE_Effect_Match_Name { "OLM Color Key" },
		AE_Reserved_Info { 0 }
	}
};
