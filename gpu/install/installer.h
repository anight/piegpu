//
// installer.h
//
// Puts files on the RPi's SD card for the host (web/installer: the page that
// installs piegpu), over the USB serial link (devtools/devlink) in text
// lines, between the log's lines:
//
//   PGI INFO                     #PGI INFO board=<name, _ for spaces> rev=<hex>
//                                  ram=<MB> card=1 size=<MB> fs=FAT32 free=<KB>
//                                  files=<name>:<bytes>,...   (card=0 error=...)
//                                  [fw=... fwgit=... fwbuilt=... fwconfig=...: the build
//                                  in the card's kernel: kernel.img, or kernel8.img
//                                  on a Zero 2 W; build_info.h]
//   PGI PUT <name> <bytes> <crc> #PGI OK       a file is coming (CRC-32, hex)
//   PGI DATA <base64>            #PGI OK <bytes so far>   (each line answered)
//   PGI END                      #PGI OK <name>   written as <name>.new, the
//                                  size and CRC checked, then renamed
//   PGI READ <name>              #PGI FILE <name> <base64>  (up to 12 KB; "-"
//                                  if there's none)
//   PGI FORMAT                   #PGI OK   the card erased: one partition, FAT32
//                                  (a blank card; the Pi's boot ROM reads FAT)
//   PGI REBOOT                   #PGI OK, then a reboot (from the card)
//
// Errors: #PGI ERR <what>. The data is base64 so that it can't hold the link's
// magic strings (piegpu-reboot, piegpu-stream: kernel.img has them), and
// each line waits for its answer: the text path has no flow control.
//
#ifndef _gpu_install_installer_h
#define _gpu_install_installer_h

#include <circle/interrupt.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <fatfs/ff.h>
#include <SDCard/emmc.h>
#include <devlink.h>

class CInstaller
{
public:
	static const unsigned MaxLine = 16 * 1024;	// DATA lines: under 8 KB (the USB serial queue)

	CInstaller (CInterruptSystem *pInterrupt, CTimer *pTimer, CDevLink *pDevLink);
	~CInstaller (void);

	// a line from the host that starts with "PGI " (no line end)
	void Command (char *pLine);

private:
	boolean InitCard (void);
	boolean Mount (void);
	void Format (void);
	void Info (void);
	void Put (const char *pName, const char *pBytes, const char *pCRC);
	void Data (const char *pBase64);
	void End (void);
	void Read (const char *pName);
	void Abort (void);

	static CString Board (void);
	CString CardBuildInfo (void);
	void Reply (const char *pFormat, ...);

	static boolean ValidName (const char *pName);

	CInterruptSystem *m_pInterrupt;
	CTimer *m_pTimer;
	CDevLink *m_pDevLink;

	CEMMCDevice *m_pEMMC;
	boolean m_bMounted;
	FATFS m_FileSystem;

	boolean m_bOpen;
	FIL m_File;
	char m_Name[64];
	char m_TempName[72];
	u32 m_nBytes;				// the file's, as announced
	u32 m_nCRC;				// announced
	u32 m_nWritten;
	u32 m_nRunningCRC;

	u8 *m_pBuffer;				// decoded DATA, READ's file
};

#endif
