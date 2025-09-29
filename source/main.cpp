// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (c) 2005 - 2013 Michael "Chishm" Chisholm
// Copyright (c) 2005 - 2013 Dave "WinterMute" Murphy
// Copyright (c) 2005 - 2013 Claudio "sverx"
// Copyright (c) 2024 Evie "Pk11"

#include "iconTitle.h"
#include "link.h"
#include "nds/arm9/console.h"
#include "nds_loader_arm9.h"
#include "version.h"

#include <fat.h>
#include <nds.h>
#include <unistd.h>

//---------------------------------------------------------------------------------
void waitforA(void) {
//---------------------------------------------------------------------------------
	while (pmMainLoop()) {
		swiWaitForVBlank();
		scanKeys();
		if (keysHeld() & KEY_A) break;
	}
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------

	// overwrite reboot stub identifier
	// so tapping power on DSi returns to DSi menu
	pmClearResetJumpTarget();

	// install exception stub
	defaultExceptionHandler();

	iconTitleInit();

	// Subscreen as a console
	videoSetModeSub(MODE_0_2D);
	vramSetBankH(VRAM_H_SUB_BG);
	consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 15, 0, false, true);

	if (!fatInitDefault()) {
		iprintf("fatinitDefault failed!\n");
		waitforA();
		return -1;
	}

	keysSetRepeat(25,5);

	mkdir("/nds", 0777);
	chdir("/nds");

	while(pmMainLoop()) {
		consoleClear();
		iprintf("================================");
		iprintf("dslink-delta " VER_NUMBER "\n");

		char filename[256];
		char arg0[256];
		bool ret = receive(filename, arg0);

		iprintf("================================");
		if(!ret) {
			iprintf("!!Failed!!\n");
			waitforA();
			return 1;
		}
		iprintf("Running:\n- %s\n", filename);
		iprintf("Args:\n- %s\n", arg0);

		const char *args[] {arg0};

		// Try to run the NDS file with the given arguments
		int err = runNdsFile(filename, sizeof(args) / sizeof(args[0]), args);
		iprintf("Start %s failed. Error %i\n", filename, err);

		waitforA();
	}

	return 0;
}
