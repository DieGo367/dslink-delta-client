// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (c) 2024 Evie "Pk11"

#include "link.h"
#include "nds/interrupts.h"

#include <nds.h>
#include <dswifi9.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <zlib.h>
#include "xdelta3.h"

#define RECV_MAGIC_3DS "3dsboot"
#define SEND_MAGIC_3DS "boot3ds"
#define RECV_MAGIC_DELTA "dslink-delta-host"
#define SEND_MAGIC_DELTA "dslink-delta-client"
#define PORT 17491

#define CHUNK_SIZE (16 * 1024)
unsigned char in[CHUNK_SIZE];
unsigned char out[CHUNK_SIZE];
static volatile size_t filelen, filetotal;

static int recvall(int sock, void* buffer, int size, int flags) {
	u8 *ptr = (u8 *)buffer;
	int len, sizeleft = size;

	while(sizeleft) {
		len = recv(sock, ptr, sizeleft, flags);
		if(!len) {
			size = 0;
			break;
		} else if(len < 0) {
			if(errno != EAGAIN && errno != EWOULDBLOCK) {
				iprintf("recv %d\n", errno);
				break;
			}
		} else {
			sizeleft -= len;
			ptr += len;
		}
	}
	return size;
}

static int receiveAndDecompress(int sock, FILE* fh, size_t filesize) {
	int ret;
	unsigned have;
	z_stream strm;
	size_t chunksize;

	// allocate inflate state
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit(&strm);
	if(ret != Z_OK) {
		iprintf("inflateInit %d\n", ret);
		return ret;
	}

	size_t total = 0;
	// decompress until deflate stream ends or end of file
	do {
		int len = recvall(sock, &chunksize, 4, 0);
		if(len != 4) {
			inflateEnd(&strm);
			iprintf("chunksize %d\n", len);
			return Z_DATA_ERROR;
		}

		strm.avail_in = recvall(sock,in,chunksize,0);
		if(strm.avail_in == 0) {
			inflateEnd(&strm);
			iprintf("closed %d\n", 0);
			return Z_DATA_ERROR;
		}

		strm.next_in = in;

		// run inflate() on input until output buffer not full
		do {
			strm.avail_out = CHUNK_SIZE;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH);

			switch(ret) {
				case Z_NEED_DICT:
					ret = Z_DATA_ERROR; // and fall through
				case Z_DATA_ERROR:
				case Z_MEM_ERROR:
				case Z_STREAM_ERROR:
					inflateEnd(&strm);
					iprintf("inflate %d\n", ret);
					return ret;
			}

			have = CHUNK_SIZE - strm.avail_out;

			if(fwrite(out, 1, have, fh) != have || ferror(fh)) {
				inflateEnd(&strm);
				iprintf("fwrite");
				return Z_ERRNO;
			}

			total += have;
			filetotal = total;
			iprintf("Progress: %zu (%d%%)\r", total, (100 * total) / filesize);
			//netloader_draw_progress();
		} while(strm.avail_out == 0);

		// done when inflate() says it's done
	} while(ret != Z_STREAM_END);

	// clean up and return
	inflateEnd(&strm);
	iprintf("Done!                           ");
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

static int receiveAndPatch(int sock, FILE *outFile, FILE *srcFile, size_t filesize) {
	xd3_stream stream;
	xd3_config config;
	xd3_init_config(&config, XD3_ADLER32);
	config.winsize = 64 * 1024;
	if (xd3_config_stream(&stream, &config) != 0) {
		iprintf("Error initializing xdelta stream\n");
		return -1;
	}

	xd3_source source = {0};
	source.name = "src";
	source.ioh = srcFile;
	source.blksize = CHUNK_SIZE;
	source.curblkno = (xoff_t) -1;
	source.curblk = NULL;
	xd3_set_source(&stream, &source);

	int retval = 0, len, chunksize = -1;
	int status = XD3_INPUT;
	size_t total = 0;

	while (total < filesize || status != XD3_INPUT) {
		switch (status) {
		case XD3_INPUT:
			len = recvall(sock, &chunksize, 4, 0);
			if (len != 4) {
				iprintf("chunksize\n");
				retval = -1;
				goto xdelta_cleanup;
			}
			len = recvall(sock, in, chunksize, 0);
			if (len == 0 || len != chunksize) {
				iprintf("closed \n");
				retval = -1;
				goto xdelta_cleanup;
			}
			xd3_avail_input(&stream, in, chunksize);
			break;
		case XD3_OUTPUT:
			if (fwrite(stream.next_out, 1, stream.avail_out, outFile) != stream.avail_out) {
				iprintf("fwrite\n");
				retval = -1;
				goto xdelta_cleanup;
			}
			total += stream.avail_out;
			filetotal = total;
			iprintf("Progress: %zu (%d%%)\r", total, (100 * total) / filesize);
			xd3_consume_output(&stream);
			break;
		case XD3_GETSRCBLK:
			fseek(srcFile, source.blksize * source.getblkno, SEEK_SET);
			source.onblk = fread(out, 1, source.blksize, srcFile);
			source.curblk = out;
			source.curblkno = source.getblkno;
			break;
		case XD3_GOTHEADER:
		case XD3_WINSTART:
			break;
		case XD3_WINFINISH:
			if (total == filesize) xd3_set_flags(&stream, XD3_FLUSH | stream.flags);
			break;
		default:
			iprintf("xdelta error!\n");
			retval = status;
			goto xdelta_cleanup;
		}
		status = xd3_decode_input(&stream);
	}

xdelta_cleanup:
	if (xd3_close_stream(&stream) != 0) {
		iprintf("Something wrong when closing stream\n");
	}
	xd3_free_stream(&stream);
	
	if (retval == 0) iprintf("Done!                           ");
	return retval;
}

//---------------------------------------------------------------------------------
bool receive(char *filename, char *arg0) {
//---------------------------------------------------------------------------------
	if(!filename) {
		iprintf("filename null\n");
		return false;
	}

	iprintf("Connecting...\r");
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
	if (bind(sock_tcp, (struct sockaddr *)&sa_tcp, sizeof(sa_tcp)) < 0) {
		iprintf(" TCP socket error\n");
		return false;
	}
	int i = 1;
	ioctl(sock_tcp, FIONBIO, &i);
	ioctl(sock_udp, FIONBIO, &i);
	listen(sock_tcp,2);

	u32 dummy;
	int sock_tcp_remote;
	char recvbuf[256];
	int spinPos = 0;
	const char *spinner = "|/-\\";
	const int spinLen = strlen(spinner);

	bool hostIsDelta = false;

	while(pmMainLoop()) {
		swiWaitForVBlank();
		iprintf("Searching... %c\r", spinner[spinPos >> 2]);
		spinPos = (spinPos + 1) % (spinLen << 2);

		int len = recvfrom(sock_udp, recvbuf, sizeof(recvbuf), 0, (struct sockaddr*) &sa_udp_remote, &dummy);

		if(len!=-1) {
			if (strncmp(recvbuf, RECV_MAGIC_3DS, sizeof(RECV_MAGIC_3DS) - 1) == 0) {
				sa_udp_remote.sin_family = AF_INET;
				sa_udp_remote.sin_port = htons(PORT);
				sendto(sock_udp, SEND_MAGIC_3DS, sizeof(SEND_MAGIC_3DS) - 1, 0, (struct sockaddr*) &sa_udp_remote, sizeof(sa_udp_remote));
			}
			else if (strncmp(recvbuf, RECV_MAGIC_DELTA, sizeof(RECV_MAGIC_DELTA) -1) == 0) {
				sa_udp_remote.sin_family = AF_INET;
				sa_udp_remote.sin_port = htons(PORT);
				sendto(sock_udp, SEND_MAGIC_DELTA, sizeof(SEND_MAGIC_DELTA) - 1, 0, (struct sockaddr*) &sa_udp_remote, sizeof(sa_udp_remote));
				hostIsDelta = true;
			}
		}

		sock_tcp_remote = accept(sock_tcp, (struct sockaddr *)&sa_tcp, &dummy);
		if(sock_tcp_remote != -1) {
			int response = 0;
			u32 len;

			bool deltaMode = false;
			if (hostIsDelta) {
				u8 mode;
				len = recvall(sock_tcp_remote, &mode, sizeof(u8), 0);
				if (len != sizeof(u8) || mode > 2) {
					iprintf("mode %d\n", errno);
					return false;
				}
				deltaMode = mode;
			}

			u32 namelen;
// SPDX-FileNotice: Modified from the original version by the BlocksDS project.
			len = recvall(sock_tcp_remote, &namelen, 4, 0);
			if(len != 4 || namelen >= 256) {
				iprintf("namelen %d\n", errno);
				return false;
			}

			len = recvall(sock_tcp_remote, recvbuf, namelen, 0);
			if(len != namelen) {
				iprintf("name %d\n", errno);
				return false;
			}
			recvbuf[namelen] = 0;
			sniprintf(filename, 256, "%s:/nds/%s", isDSiMode() ? "sd" : "fat", recvbuf);

			len = recvall(sock_tcp_remote, (int*)&filelen, 4, 0);
			if(len != 4) {
				iprintf("filelen %d\n", errno);
				return false;
			}

			iprintf("Receiving %s,\n          %d bytes\n", filename, filelen);

			FILE *sourceFile = NULL;
			if (deltaMode) {
				sourceFile = fopen(filename, "rb");
				if (!sourceFile) {
					iprintf("Failed to open %s\n", filename);
					response = -4;
					deltaMode = false;
				}
				else {
					uint32_t hostChecksum;
					len = recvall(sock_tcp_remote, &hostChecksum, 4, 0);
					if (len != 4) {
						iprintf("hostChecksum %d\n", errno);
						return false;
					}
					uint32_t checksum = adler32(0, NULL, 0);
					while (!feof(sourceFile)) {
						size_t read = fread(in, 1, CHUNK_SIZE, sourceFile);
						checksum = adler32(checksum, in, read);
					}
					fseek(sourceFile, 0, SEEK_SET);
					if (checksum != hostChecksum) {
						iprintf("Mismatched checksum\n");
						response = -5;
						deltaMode = false;
						fclose(sourceFile);
						sourceFile = NULL;
					}
				}
			}

			FILE *outfile = fopen(deltaMode ? "dslink.out" : filename, "wb");
			if(!outfile) {
				iprintf("Failed to open %s\n", deltaMode ? "dslink.out" : filename);
				response = -1;
			}

			send(sock_tcp_remote, &response, sizeof(response), 0);

			int res = 0;
			if (deltaMode) res = receiveAndPatch(sock_tcp_remote, outfile, sourceFile, filelen);
			else res = receiveAndDecompress(sock_tcp_remote, outfile, filelen);

			fclose(outfile);
			if (sourceFile) fclose(sourceFile);

			if (deltaMode) {
				if (res != 0) {
					iprintf("delta patch failed %d\n", res);
					return false;
				}
				remove(filename);
				rename("dslink.out", filename);
			}
			else if(res != Z_OK) {
				iprintf("decompress failed %d\n", res);
				return false;
			}

			send(sock_tcp_remote, &response, sizeof(response), 0);

			u32 cmdlen;
			len = recvall(sock_tcp_remote, &cmdlen, 4, 0);
			if(len == 4 && cmdlen <= sizeof(recvbuf)) {
				len = recvall(sock_tcp_remote, recvbuf, cmdlen, 0);
				if(len == cmdlen) {
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
