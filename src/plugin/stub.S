	.set noreorder

#include "pspstub.s"

	STUB_START "sceRtc_driver",0x00010011,0x00020005
	STUB_FUNC  0xC2DDBEB5,sceRtcGetAlarmTick
	STUB_FUNC  0x7D1FBED3,sceRtcSetAlarmTick
	STUB_END

