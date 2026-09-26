#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

// PiPL identity decoded from the Windows .aex resource section:
// Name "OLM DirectionalBlur", Category "OLM Plug-ins",
// Match Name "OLM Directional Blur", entry "entryPointFunc" (Win) / "EffectMain" (Mac),
// Spec 13.29, Version 0x00088800 (1.1.1), OutFlags 0x06000040, OutFlags2 0x08001408.
resource 'PiPL' (16000) {
	{	/* array properties: 12 elements */
		/* [1] */
		Kind {
			AEEffect
		},
		/* [2] */
		Name {
			"OLM DirectionalBlur"
		},
		/* [3] */
		Category {
			"OLM Plug-ins"
		},
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
		/* [6] */
		AE_PiPL_Version {
			2,
			0
		},
		/* [7] */
		AE_Effect_Spec_Version {
			13,
			29
		},
		/* [8] */
		AE_Effect_Version {
			559104	/* 1.1.1 */
		},
		/* [9] */
		AE_Effect_Info_Flags {
			0
		},
		/* [10] */
		AE_Effect_Global_OutFlags {
			0x06000040
		},
		AE_Effect_Global_OutFlags_2 {
			0x08001408
		},
		/* [11] */
		AE_Effect_Match_Name {
			"OLM Directional Blur"
		},
		/* [12] */
		AE_Reserved_Info {
			0
		}
	}
};
