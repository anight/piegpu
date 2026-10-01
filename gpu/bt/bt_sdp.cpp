//
// bt_sdp.cpp
//
#include "bt_sdp.h"
#include <circle/util.h>

#define RECORD_HANDLE		0x00010001

#define PDU_ERROR		0x01
#define PDU_SEARCH		0x02
#define PDU_SEARCH_RSP		0x03
#define PDU_ATTRIBUTES		0x04
#define PDU_ATTRIBUTES_RSP	0x05
#define PDU_SEARCH_ATTRIBUTES	0x06
#define PDU_SEARCH_ATTR_RSP	0x07

// The record's attributes, in the order of their IDs: each its ID (a 16-bit
// unsigned: 09) and its value. 35 nn: a sequence of nn bytes; 19: a 16-bit
// UUID; 0A: a 32-bit unsigned
static const struct { u16 nId; u8 nBytes; u8 Value[20]; } Attributes[] =
{
	{0x0000, 5, {0x0A, 0x00, 0x01, 0x00, 0x01}},			// the record's handle
	{0x0001, 5, {0x35, 3, 0x19, 0x11, 0x0A}},			// its class: AudioSource
	{0x0004, 18, {0x35, 16, 0x35, 6, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,	// L2CAP, PSM 0x19
			       0x35, 6, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03}},	// AVDTP 1.3
	{0x0005, 5, {0x35, 3, 0x19, 0x10, 0x02}},			// browsable: the public root
	{0x0009, 10, {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03}},	// the profile: A2DP 1.3
	{0x0311, 3, {0x09, 0x00, 0x01}},				// features: a player
};
// the UUIDs in it (a search finds the record if all it asks for are among these)
static const u16 RecordUUIDs[] = {0x110A, 0x0100, 0x0019, 0x1002, 0x110D};

// a data element's header: its type, where its data starts, how long it is
static boolean Element (const u8 *p, unsigned n, unsigned *pType, unsigned *pStart, unsigned *pLength)
{
	if (n < 1)
	{
		return FALSE;
	}
	*pType = p[0] >> 3;
	unsigned nSize = p[0] & 7;
	if (nSize <= 4)
	{
		*pStart = 1;
		*pLength = *pType == 0 ? 0 : 1U << nSize;
	}
	else if (nSize == 5 && n >= 2)
	{
		*pStart = 2;
		*pLength = p[1];
	}
	else if (nSize == 6 && n >= 3)
	{
		*pStart = 3;
		*pLength = p[1] << 8 | p[2];
	}
	else if (nSize == 7 && n >= 5)
	{
		*pStart = 5;
		*pLength = (unsigned) p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
	}
	else
	{
		return FALSE;
	}
	return *pStart + *pLength <= n;
}

// A search pattern (a sequence of UUIDs): does the record have them all?
// *pUsed: the pattern's length
static boolean Matches (const u8 *p, unsigned n, unsigned *pUsed)
{
	static const u8 Base[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
	unsigned nType, nStart, nLength;
	if (!Element (p, n, &nType, &nStart, &nLength) || nType != 6)
	{
		*pUsed = n;
		return FALSE;
	}
	*pUsed = nStart + nLength;
	boolean bAll = nLength > 0;
	for (unsigned i = nStart; i < nStart + nLength; )
	{
		unsigned nT, nS, nL;
		if (!Element (p + i, nStart + nLength - i, &nT, &nS, &nL))
		{
			return FALSE;
		}
		const u8 *q = p + i + nS;
		u32 nUUID = 0xFFFFFFFF;
		if (nT == 3 && nL == 2)
		{
			nUUID = q[0] << 8 | q[1];
		}
		else if (nT == 3 && (nL == 4 || (nL == 16 && memcmp (q + 4, Base, 12) == 0)))
		{
			nUUID = (u32) q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
		}
		boolean bFound = FALSE;
		for (unsigned k = 0; k < sizeof RecordUUIDs / sizeof RecordUUIDs[0]; k++)
		{
			bFound = bFound || nUUID == RecordUUIDs[k];
		}
		bAll = bAll && bFound;
		i += nS + nL;
	}
	return bAll;
}

// The record's attributes whose IDs an attribute ID list asks for (IDs, or
// ranges of them), as a sequence. \return its length; *pUsed: the list's
static unsigned Listed (const u8 *p, unsigned n, u8 *pOut, unsigned *pUsed)
{
	unsigned nType, nStart, nLength;
	if (!Element (p, n, &nType, &nStart, &nLength) || nType != 6)
	{
		*pUsed = n;
		nStart = nLength = 0;
	}
	else
	{
		*pUsed = nStart + nLength;
	}
	unsigned nOut = 2;
	for (unsigned a = 0; a < sizeof Attributes / sizeof Attributes[0]; a++)
	{
		boolean bAsked = FALSE;
		for (unsigned i = nStart; i < nStart + nLength && !bAsked; )
		{
			unsigned nT, nS, nL;
			if (!Element (p + i, nStart + nLength - i, &nT, &nS, &nL))
			{
				break;
			}
			const u8 *q = p + i + nS;
			if (nT == 1 && nL == 2)
			{
				bAsked = Attributes[a].nId == (q[0] << 8 | q[1]);
			}
			else if (nT == 1 && nL == 4)
			{
				bAsked = Attributes[a].nId >= (q[0] << 8 | q[1]) && Attributes[a].nId <= (q[2] << 8 | q[3]);
			}
			i += nS + nL;
		}
		if (bAsked)
		{
			pOut[nOut++] = 0x09;
			pOut[nOut++] = (u8) (Attributes[a].nId >> 8);
			pOut[nOut++] = (u8) Attributes[a].nId;
			memcpy (pOut + nOut, Attributes[a].Value, Attributes[a].nBytes);
			nOut += Attributes[a].nBytes;
		}
	}
	pOut[0] = 0x35;
	pOut[1] = (u8) (nOut - 2);
	return nOut;
}

unsigned SDPAnswer (const u8 *pRequest, unsigned nBytes, u8 *pAnswer, unsigned nMax)
{
	if (nBytes < 5 || nMax < 128)
	{
		return 0;
	}
	const u8 *p = pRequest + 5;
	unsigned n = pRequest[3] << 8 | pRequest[4];
	if (5 + n > nBytes)
	{
		n = nBytes - 5;
	}
	u8 *pOut = pAnswer + 5;
	unsigned nOut = 0, nUsed;
	u8 nCode = PDU_ERROR;

	switch (pRequest[0])
	{
	case PDU_SEARCH:				// a pattern, how many at most: the handles
	{
		boolean bFound = Matches (p, n, &nUsed);
		nCode = PDU_SEARCH_RSP;
		pOut[0] = pOut[2] = 0;
		pOut[1] = pOut[3] = bFound ? 1 : 0;
		nOut = 4;
		if (bFound)
		{
			pOut[4] = (u8) (RECORD_HANDLE >> 24);
			pOut[5] = (u8) (RECORD_HANDLE >> 16);
			pOut[6] = (u8) (RECORD_HANDLE >> 8);
			pOut[7] = (u8) RECORD_HANDLE;
			nOut = 8;
		}
		pOut[nOut++] = 0;			// (all of it: nothing to continue)
		break;
	}

	case PDU_ATTRIBUTES:				// a handle, how many bytes at most, the IDs: the attributes
		if (n >= 6 && ((u32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]) == RECORD_HANDLE)
		{
			nCode = PDU_ATTRIBUTES_RSP;
			unsigned nList = Listed (p + 6, n - 6, pOut + 2, &nUsed);
			pOut[0] = 0;
			pOut[1] = (u8) nList;
			nOut = 2 + nList;
			pOut[nOut++] = 0;
		}
		else
		{
			pOut[0] = 0;
			pOut[1] = 0x02;			// (no such record)
			nOut = 2;
		}
		break;

	case PDU_SEARCH_ATTRIBUTES:			// a pattern, how many bytes at most, the IDs: each record's attributes
	{
		boolean bFound = Matches (p, n, &nUsed);
		nCode = PDU_SEARCH_ATTR_RSP;
		unsigned nList = 0, nSkip;
		if (bFound && nUsed + 2 <= n)
		{
			nList = Listed (p + nUsed + 2, n - nUsed - 2, pOut + 4, &nSkip);
		}
		pOut[0] = 0;
		pOut[1] = (u8) (2 + nList);
		pOut[2] = 0x35;
		pOut[3] = (u8) nList;
		nOut = 4 + nList;
		pOut[nOut++] = 0;
		break;
	}

	default:
		pOut[0] = 0;
		pOut[1] = 0x03;				// (not a request we know)
		nOut = 2;
		break;
	}

	pAnswer[0] = nCode;
	pAnswer[1] = pRequest[1];			// the request's transaction
	pAnswer[2] = pRequest[2];
	pAnswer[3] = (u8) (nOut >> 8);
	pAnswer[4] = (u8) nOut;
	return 5 + nOut;
}
