// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (c) 2005 - 2010 Michael "Chishm" Chisholm
// Copyright (c) 2005 - 2010 Dave "WinterMute" Murphy

#ifndef NDS_LOADER_ARM9_H
#define NDS_LOADER_ARM9_H


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	RUN_NDS_OK = 0,
	RUN_NDS_STAT_FAILED,
	RUN_NDS_GETCWD_FAILED,
	RUN_NDS_PATCH_DLDI_FAILED,
} eRunNdsRetCode;

#define LOAD_DEFAULT_NDS 0

eRunNdsRetCode runNdsFile(const char* filename, int argc, const char** argv);

bool installBootStub(bool havedsiSD);
void installExcptStub(void);

#ifdef __cplusplus
}
#endif

#endif // NDS_LOADER_ARM7_H
