// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (c) 2005 - 2013 Michael "Chishm" Chisholm
// Copyright (c) 2005 - 2013 Dave "WinterMute" Murphy
// Copyright (c) 2005 - 2013 Claudio "sverx"
// Copyright (c) 2024 Evie "Pk11"

#include <nds.h>
#include <dswifi9.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <fat.h>
#include <sys/stat.h>
#include <stdio.h>
#include <limits.h>
#include <zlib.h>

#include <string.h>
#include <unistd.h>

#include "calico/nds/pm.h"
#include "hbmenu_banner.h"
#include "iconTitle.h"
#include "nds/arm9/sassert.h"
#include "nds/interrupts.h"
#include "nds/system.h"
#include "nds_loader_arm9.h"

#define ZLIB_CHUNK (16 * 1024)

static volatile size_t filelen, filetotal;

//---------------------------------------------------------------------------------
void stop(void) {
//---------------------------------------------------------------------------------
	while (pmMainLoop()) {
		swiWaitForVBlank();
	}
}

static int recvall(int sock, void* buffer, int size, int flags)
{
	u8 *ptr = (u8 *)buffer;
	int len, sizeleft = size;

	while (sizeleft)
	{
		len = recv(sock, ptr, sizeleft, flags);
		if (!len)
		{
			size = 0;
			break;
		} else if (len < 0)
		{
			if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
				iprintf("recv %d", errno);
				break;
			}
		} else
		{
			sizeleft -= len;
			ptr += len;
		}
	}
	return size;
}

static int receiveAndDecompress(int sock, FILE* fh, size_t filesize)
{
	static unsigned char in[ZLIB_CHUNK];
	static unsigned char out[ZLIB_CHUNK];

	int ret;
	unsigned have;
	z_stream strm;
	size_t chunksize;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit(&strm);
	if (ret != Z_OK)
	{
		iprintf("inflateInit %d\n", ret);
		return ret;
	}

	size_t total = 0;
	// decompress until deflate stream ends or end of file
	do
	{
		int len = recvall(sock, &chunksize, 4, 0);

		if (len != 4)
		{
			inflateEnd(&strm);
			iprintf("chunksize %d\n", len);
			return Z_DATA_ERROR;
		}

		strm.avail_in = recvall(sock,in,chunksize,0);

		if (strm.avail_in == 0)
		{
			inflateEnd(&strm);
			iprintf("closed %d\n", 0);
			return Z_DATA_ERROR;
		}

		strm.next_in = in;

		// run inflate() on input until output buffer not full
		do
		{
			strm.avail_out = ZLIB_CHUNK;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH);

			switch (ret)
			{
				case Z_NEED_DICT:
					ret = Z_DATA_ERROR; // and fall through
				case Z_DATA_ERROR:
				case Z_MEM_ERROR:
				case Z_STREAM_ERROR:
					inflateEnd(&strm);
					iprintf("inflate %d\n", ret);
					return ret;
			}

			have = ZLIB_CHUNK - strm.avail_out;

			if (fwrite(out, 1, have, fh) != have || ferror(fh))
			{
				inflateEnd(&strm);
				iprintf("fwrite");
				return Z_ERRNO;
			}

			total += have;
			filetotal = total;
			//sprintf(progress,"%zu (%d%%)",total, (100 * total) / filesize);
			//netloader_draw_progress();
		} while (strm.avail_out == 0);

		// done when inflate() says it's done
	} while (ret != Z_STREAM_END);

	// clean up and return
	inflateEnd(&strm);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

//---------------------------------------------------------------------------------
int loadNDS(int socket, u32 remote, char *filename) {
//---------------------------------------------------------------------------------
	

	return true;
}

//---------------------------------------------------------------------------------
bool receive(char *filename, char *arg0) {
//---------------------------------------------------------------------------------
	if(!filename) {
		iprintf("filename null\n");
		return false;
	}

	if(!Wifi_InitDefault(WFC_CONNECT)) {
		iprintf("Failed to connect!\n");
		return false;
	}

	struct in_addr ip, gateway, mask, dns1, dns2;
	ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
	iprintf("Connected: %s\n",inet_ntoa(ip));
	
	int sock_udp = socket(PF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa_udp, sa_udp_remote;

	sa_udp.sin_family = AF_INET;
	sa_udp.sin_addr.s_addr = INADDR_ANY;
	sa_udp.sin_port = htons(17491);

	if(bind(sock_udp, (struct sockaddr*) &sa_udp, sizeof(sa_udp)) < 0) {
		iprintf(" UDP socket error\n");
		return false;
	}

	struct sockaddr_in sa_tcp;
	sa_tcp.sin_addr.s_addr = INADDR_ANY;
	sa_tcp.sin_family = AF_INET;
	sa_tcp.sin_port = htons(17491);
	int sock_tcp = socket(AF_INET,SOCK_STREAM,0);
	bind(sock_tcp,(struct sockaddr *)&sa_tcp,sizeof(sa_tcp));
	int i = 1;
	ioctl(sock_tcp, FIONBIO, &i);
	ioctl(sock_udp, FIONBIO, &i);
	listen(sock_tcp,2);

	u32 dummy;
	int sock_tcp_remote;
	char recvbuf[256];

	while(pmMainLoop()) {
		int len = recvfrom(sock_udp, recvbuf, sizeof(recvbuf), 0, (struct sockaddr*) &sa_udp_remote, &dummy);

		if (len!=-1) {
			if (strncmp(recvbuf,"3dsboot",strlen("3dsboot")) == 0) {
				sa_udp_remote.sin_family=AF_INET;
				sa_udp_remote.sin_port=htons(17491);
				sendto(sock_udp, "boot3ds", strlen("boot3ds"), 0, (struct sockaddr*) &sa_udp_remote,sizeof(sa_udp_remote));
			}
		}

		sock_tcp_remote = accept(sock_tcp,(struct sockaddr *)&sa_tcp,&dummy);
		if (sock_tcp_remote != -1) {
			int response = 0;
			u32 namelen, len;

			len = recvall(sock_tcp_remote, &namelen, 4, 0);
			if (len != 4 || namelen >= 256)
			{
				iprintf("namelen %d", errno);
				return false;
			}

			len = recvall(sock_tcp_remote, recvbuf, namelen, 0);
			if (len != namelen)
			{
				iprintf("name %d", errno);
				return false;
			}
			recvbuf[namelen] = 0;
			sniprintf(filename, 256, "%s:/nds/%s", isDSiMode() ? "sd" : "fat", recvbuf);

			len = recvall(sock_tcp_remote, (int*)&filelen, 4, 0);
			if (len != 4)
			{
				iprintf("filelen %d", errno);
				return false;
			}

			iprintf("Receiving %s, %d bytes\n", filename, filelen);

			FILE *outfile = fopen(filename, "wb");
			if(!outfile) {
				iprintf("Failed to open %s", filename);
				response = -1;
			}

			send(sock_tcp_remote, &response, sizeof(response), 0);

			int res = receiveAndDecompress(sock_tcp_remote, outfile, filelen);
			fclose(outfile);
			if(res != Z_OK) {
				iprintf("decompress failed %d", res);
				return false;
			}

			send(sock_tcp_remote, &response, sizeof(response), 0);

			u32 cmdlen;
			len = recvall(sock_tcp_remote, &cmdlen, 4, 0);
			if (len == 4 && cmdlen <= sizeof(recvbuf)) {
				len = recvall(sock_tcp_remote, recvbuf, cmdlen, 0);
				if (len == cmdlen) {
					recvbuf[cmdlen] = 0;
					if(memcmp(recvbuf, "sdmc:/3ds/", 10) == 0) {
						sniprintf(arg0, 256, "%s:/nds/%s", isDSiMode() ? "sd" : "fat", recvbuf + 10);
					} else {
						strcpy(arg0, recvbuf);
					}
				}
			}
			shutdown(sock_tcp_remote, 0);
			closesocket(sock_tcp_remote);

			return true;
		}
	}

	return false;
}


//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------

	// overwrite reboot stub identifier
	// so tapping power on DSi returns to DSi menu
	pmClearResetJumpTarget();

	// install exception stub
	installExcptStub();

	iconTitleInit();

	// Subscreen as a console
	videoSetModeSub(MODE_0_2D);
	vramSetBankH(VRAM_H_SUB_BG);
	consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 15, 0, false, true);

	if (!fatInitDefault()) {
		iprintf("fatinitDefault failed!\n");
		stop();
	}

	keysSetRepeat(25,5);

	mkdir("/nds", 0777);
	chdir("/nds");

	while(pmMainLoop()) {
		char filename[256];
		char arg0[256];
		receive(filename, arg0);

		iprintf("================================");
		iprintf("Running:\n- %s\n", filename);
		iprintf("Args:\n- %s\n", arg0);

		const char *args[] {arg0};

		// Try to run the NDS file with the given arguments
		int err = runNdsFile(filename, sizeof(args) / sizeof(args[0]), args);
		iprintf("Start %s failed. Error %i\n", filename, err);

		while (pmMainLoop()) {
			swiWaitForVBlank();
			scanKeys();
			if (!(keysHeld() & KEY_A)) break;
		}

	}

	return 0;
}
