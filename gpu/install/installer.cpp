//
// installer.cpp
//
#include "installer.h"
#include "../build_info.h"
#include <circle/logger.h>
#include <circle/machineinfo.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/util.h>
#include <runlog.h>
#include <stdarg.h>

LOGMODULE ("install");

#define DRIVE		"SD:"
#define BUFFER_BYTES	(12 * 1024)

// this build's kernel on the card (the firmware's default name: Zero 32 bit,
// Zero 2 W 64 bit)
#if AARCH == 64
	#define KERNEL		"kernel8.img"
#else
	#define KERNEL		"kernel.img"
#endif

// the files INFO reports
static const char *const Files[] =
{
	"bootcode.bin", "start.elf", "fixup.dat", "kernel.img", "kernel8.img", "config.txt", "cmdline.txt"
};

static u32 CRCTable[256];

static u32 CRC32 (u32 nCRC, const u8 *p, unsigned n)	// zlib's: CRC32 (0, ...)
{
	if (!CRCTable[1])
	{
		for (u32 i = 0; i < 256; i++)
		{
			u32 c = i;
			for (unsigned k = 0; k < 8; k++)
			{
				c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
			}
			CRCTable[i] = c;
		}
	}
	nCRC = ~nCRC;
	while (n--)
	{
		nCRC = CRCTable[(nCRC ^ *p++) & 0xFF] ^ (nCRC >> 8);
	}
	return ~nCRC;
}

static const char Base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int Base64Value (char c)
{
	const char *p = strchr (Base64, c);
	return c && p ? p - Base64 : -1;
}

// the bytes, or -1 if it isn't base64 (or more than nMax)
static int Base64Decode (const char *pIn, u8 *pOut, unsigned nMax)
{
	unsigned n = 0, nBits = 0;
	u32 nAcc = 0;
	for (; *pIn && *pIn != '='; pIn++)
	{
		int v = Base64Value (*pIn);
		if (v < 0)
		{
			return -1;
		}
		nAcc = nAcc << 6 | v;
		nBits += 6;
		if (nBits >= 8)
		{
			nBits -= 8;
			if (n >= nMax)
			{
				return -1;
			}
			pOut[n++] = (u8) (nAcc >> nBits);
		}
	}
	return n;
}

static void Base64Encode (const u8 *p, unsigned n, CString *pOut)
{
	char Quad[5] = {0};
	for (unsigned i = 0; i < n; i += 3)
	{
		u32 v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		Quad[0] = Base64[v >> 18];
		Quad[1] = Base64[(v >> 12) & 63];
		Quad[2] = i + 1 < n ? Base64[(v >> 6) & 63] : '=';
		Quad[3] = i + 2 < n ? Base64[v & 63] : '=';
		pOut->Append (Quad);
	}
}

CInstaller::CInstaller (CInterruptSystem *pInterrupt, CTimer *pTimer, CDevLink *pDevLink)
:	m_pInterrupt (pInterrupt),
	m_pTimer (pTimer),
	m_pDevLink (pDevLink),
	m_pEMMC (nullptr),
	m_bMounted (FALSE),
	m_bOpen (FALSE),
	m_pBuffer (nullptr)
{
}

CInstaller::~CInstaller (void)
{
	Abort ();
	delete [] m_pBuffer;
}

void CInstaller::Command (char *pLine)
{
	char *pSave = nullptr;
	strtok_r (pLine, " ", &pSave);			// "PGI"
	const char *pCommand = strtok_r (nullptr, " ", &pSave);
	const char *pArg[3];
	for (unsigned i = 0; i < 3; i++)
	{
		pArg[i] = strtok_r (nullptr, " ", &pSave);
	}

	if (!pCommand)
	{
		Reply ("ERR no command");
	}
	else if (strcmp (pCommand, "INFO") == 0)
	{
		Info ();
	}
	else if (strcmp (pCommand, "PUT") == 0 && pArg[2])
	{
		Put (pArg[0], pArg[1], pArg[2]);
	}
	else if (strcmp (pCommand, "DATA") == 0 && pArg[0])
	{
		Data (pArg[0]);
	}
	else if (strcmp (pCommand, "END") == 0)
	{
		End ();
	}
	else if (strcmp (pCommand, "READ") == 0 && pArg[0])
	{
		Read (pArg[0]);
	}
	else if (strcmp (pCommand, "FORMAT") == 0)
	{
		Format ();
	}
	else if (strcmp (pCommand, "REBOOT") == 0)
	{
		Abort ();
		if (m_bMounted)
		{
			f_mount (nullptr, DRIVE, 0);
			m_bMounted = FALSE;
		}
		Reply ("OK");
		LOGNOTE ("Rebooting (the host asked)");
		CTimer::SimpleMsDelay (100);		// the answer goes out
		CRunLog::Restarting ();
		reboot ();
	}
	else
	{
		Reply ("ERR unknown command %s", pCommand);
	}
}

// the card's driver (on first use: a card put in after boot is found too)
boolean CInstaller::InitCard (void)
{
	if (!m_pBuffer)
	{
		m_pBuffer = new u8[BUFFER_BYTES];
	}
	if (!m_pEMMC)
	{
		m_pEMMC = new CEMMCDevice (m_pInterrupt, m_pTimer);
		if (!m_pEMMC->Initialize ())
		{
			delete m_pEMMC;			// again at the next command
			m_pEMMC = nullptr;
			Reply ("INFO %s card=0 error=no-card", (const char *) Board ());
			return FALSE;
		}
		LOGNOTE ("SD card: %llu MB", m_pEMMC->GetSize () >> 20);
	}
	return TRUE;
}

boolean CInstaller::Mount (void)
{
	if (m_bMounted)
	{
		return TRUE;
	}
	if (!InitCard ())
	{
		return FALSE;
	}
	FRESULT Result = f_mount (&m_FileSystem, DRIVE, 1);
	if (Result != FR_OK)
	{
		Reply ("INFO %s card=1 size=%llu fs=none error=%s", (const char *) Board (), m_pEMMC->GetSize () >> 20,
		       Result == FR_NO_FILESYSTEM ? "no-FAT-filesystem" : "mount-failed");
		return FALSE;
	}
	m_bMounted = TRUE;

	return TRUE;
}

void CInstaller::Info (void)
{
	if (!Mount ())
	{
		return;
	}

	DWORD nFreeClusters = 0;
	FATFS *pFS = nullptr;
	u64 nFreeKB = 0;
	if (f_getfree (DRIVE, &nFreeClusters, &pFS) == FR_OK)
	{
		nFreeKB = (u64) nFreeClusters * pFS->csize * FF_MIN_SS / 1024;
	}
	const char *pType =   m_FileSystem.fs_type == FS_FAT32 ? "FAT32"
			    : m_FileSystem.fs_type == FS_FAT16 ? "FAT16"
			    : m_FileSystem.fs_type == FS_FAT12 ? "FAT12" : "other";

	CString List;
	for (const char *pName : Files)
	{
		CString Path;
		Path.Format (DRIVE "/%s", pName);
		FILINFO Info;
		if (f_stat (Path, &Info) == FR_OK)
		{
			CString Entry;
			Entry.Format ("%s%s:%lu", List.GetLength () ? "," : "", pName, (unsigned long) Info.fsize);
			List.Append (Entry);
		}
	}

	CString Build = CardBuildInfo ();
	Reply ("INFO %s card=1 size=%llu fs=%s free=%llu files=%s%s%s", (const char *) Board (),
	       m_pEMMC->GetSize () >> 20, pType,
	       nFreeKB, List.GetLength () ? (const char *) List : "-",
	       Build.GetLength () ? " " : "", (const char *) Build);
}

// the build line in the card's kernel for this board (KERNEL; build_info.h:
// "fw=... fwbuilt=... fwconfig=..."), or "" (none there, or an older build
// without it)
CString CInstaller::CardBuildInfo (void)
{
	CString Line;
	FIL File;
	if (f_open (&File, DRIVE "/" KERNEL, FA_READ) != FR_OK)
	{
		return Line;
	}
	unsigned nSize = f_size (&File);
	u8 *pImage = nSize <= 16 * 1024 * 1024 ? new u8[nSize + 1] : nullptr;
	UINT nRead = 0;
	if (pImage && f_read (&File, pImage, nSize, &nRead) == FR_OK && nRead == nSize)
	{
		// the frame's start followed by "fw=" (this code's own copy of the
		// start is followed by other bytes)
		static const char Start[] = BUILD_INFO_START "fw=";
		const unsigned nStart = sizeof Start - 1;
		for (unsigned i = 0; i + nStart <= nSize && !Line.GetLength (); i++)
		{
			if (pImage[i] == (u8) Start[0] && memcmp (pImage + i, Start, nStart) == 0)
			{
				unsigned nFrom = i + sizeof BUILD_INFO_START - 1, j = nFrom;
				while (j < nSize && j - nFrom < 256 && pImage[j] >= ' ' && pImage[j] < 0x7F)
				{
					j++;
				}
				if (j < nSize && pImage[j] == (u8) BUILD_INFO_END)
				{
					pImage[j] = '\0';
					Line = (const char *) pImage + nFrom;
				}
			}
		}
	}
	delete [] pImage;
	f_close (&File);

	return Line;
}

// FatFs' f_mkfs: an MBR with one FAT32 partition over the card (type 0Ch,
// which the Pi's boot ROM reads)
void CInstaller::Format (void)
{
	Abort ();
	if (m_bMounted)
	{
		f_mount (nullptr, DRIVE, 0);
		m_bMounted = FALSE;
	}
	if (!InitCard ())
	{
		return;
	}
	LOGNOTE ("Formatting the SD card (FAT32)");
	const MKFS_PARM Options = {FM_FAT32, 2, 0, 0, 0};
	FRESULT Result = f_mkfs (DRIVE, &Options, m_pBuffer, BUFFER_BYTES);
	if (Result != FR_OK)
	{
		Reply ("ERR format failed (%d)", Result);
		return;
	}
	if (!Mount ())
	{
		return;
	}
	Reply ("OK");
}

void CInstaller::Put (const char *pName, const char *pBytes, const char *pCRC)
{
	Abort ();
	if (!ValidName (pName))
	{
		Reply ("ERR bad name");
		return;
	}
	if (!Mount ())
	{
		return;
	}

	strcpy (m_Name, pName);
	CString Temp;
	Temp.Format (DRIVE "/%s.new", pName);
	strcpy (m_TempName, Temp);
	m_nBytes = strtoul (pBytes, nullptr, 10);
	m_nCRC = strtoul (pCRC, nullptr, 16);
	m_nWritten = 0;
	m_nRunningCRC = 0;

	FRESULT Result = f_open (&m_File, m_TempName, FA_WRITE | FA_CREATE_ALWAYS);
	if (Result != FR_OK)
	{
		Reply ("ERR can't create %s (%d)", pName, Result);
		return;
	}
	m_bOpen = TRUE;
	Reply ("OK");
}

void CInstaller::Data (const char *pBase64)
{
	if (!m_bOpen)
	{
		Reply ("ERR no file open");
		return;
	}
	int n = Base64Decode (pBase64, m_pBuffer, BUFFER_BYTES);
	if (n < 0 || m_nWritten + n > m_nBytes)
	{
		Abort ();
		Reply ("ERR bad data");
		return;
	}
	UINT nDone = 0;
	FRESULT Result = f_write (&m_File, m_pBuffer, n, &nDone);
	if (Result != FR_OK || (int) nDone != n)
	{
		Abort ();
		Reply ("ERR write failed (%d%s)", Result, Result == FR_OK ? ", card full" : "");
		return;
	}
	m_nRunningCRC = CRC32 (m_nRunningCRC, m_pBuffer, n);
	m_nWritten += n;
	Reply ("OK %u", m_nWritten);
}

void CInstaller::End (void)
{
	if (!m_bOpen)
	{
		Reply ("ERR no file open");
		return;
	}
	m_bOpen = FALSE;
	FRESULT Result = f_close (&m_File);
	if (Result != FR_OK || m_nWritten != m_nBytes || m_nRunningCRC != m_nCRC)
	{
		f_unlink (m_TempName);
		Reply ("ERR %s: %u of %u bytes, CRC %08X (expected %08X), close %d", m_Name, m_nWritten,
		       m_nBytes, m_nRunningCRC, m_nCRC, Result);
		return;
	}

	CString Path;
	Path.Format (DRIVE "/%s", m_Name);
	f_unlink (Path);				// (there may be none)
	Result = f_rename (m_TempName, Path);
	if (Result != FR_OK)
	{
		Reply ("ERR can't rename %s (%d)", m_Name, Result);
		return;
	}
	LOGNOTE ("Wrote %s: %u bytes, CRC %08X", m_Name, m_nWritten, m_nCRC);
	Reply ("OK %s", m_Name);
}

void CInstaller::Read (const char *pName)
{
	if (!ValidName (pName))
	{
		Reply ("ERR bad name");
		return;
	}
	if (!Mount ())
	{
		return;
	}
	CString Path;
	Path.Format (DRIVE "/%s", pName);
	FIL File;
	if (f_open (&File, Path, FA_READ) != FR_OK)
	{
		Reply ("FILE %s -", pName);
		return;
	}
	UINT n = 0;
	FRESULT Result = f_read (&File, m_pBuffer, BUFFER_BYTES, &n);
	boolean bAll = f_eof (&File);
	f_close (&File);
	if (Result != FR_OK || !bAll)
	{
		Reply ("ERR can't read %s (%d%s)", pName, Result, Result == FR_OK ? ", too big" : "");
		return;
	}
	CString Data;
	Base64Encode (m_pBuffer, n, &Data);
	Reply ("FILE %s %s", pName, n ? (const char *) Data : "");
}

void CInstaller::Abort (void)
{
	if (m_bOpen)
	{
		f_close (&m_File);
		f_unlink (m_TempName);
		m_bOpen = FALSE;
	}
}

// the board, as its firmware reports it (the revision code; Circle's
// CMachineInfo decodes it): "board=Raspberry_Pi_Zero_W rev=9000c1 ram=512"
CString CInstaller::Board (void)
{
	const CMachineInfo *pInfo = CMachineInfo::Get ();
	CString Name (pInfo->GetMachineName ());
	Name.Replace (" ", "_");				// (INFO's fields are split at spaces)
	CString Fields;
	Fields.Format ("board=%s rev=%x ram=%u", (const char *) Name, pInfo->GetRevisionRaw (),
		       pInfo->GetRAMSize ());
	return Fields;
}

void CInstaller::Reply (const char *pFormat, ...)
{
	va_list Args;
	va_start (Args, pFormat);
	CString Message;
	Message.FormatV (pFormat, Args);
	va_end (Args);

	CString Line;
	Line.Format ("\n#PGI %s\n", (const char *) Message);
	m_pDevLink->Write ((const char *) Line, Line.GetLength ());
}

boolean CInstaller::ValidName (const char *pName)
{
	unsigned n = 0;
	for (const char *p = pName; *p; p++, n++)
	{
		char c = *p;
		if (!(   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		      || c == '.' || c == '_' || c == '-'))
		{
			return FALSE;
		}
	}
	return n > 0 && n < 60 && pName[0] != '.';
}
