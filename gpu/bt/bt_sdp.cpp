//
// bt_sdp.cpp
//
#include "bt_sdp.h"
#include <circle/util.h>

#define PDU_ERROR		0x01
#define PDU_SEARCH		0x02
#define PDU_SEARCH_RSP		0x03
#define PDU_ATTRIBUTES		0x04
#define PDU_ATTRIBUTES_RSP	0x05
#define PDU_SEARCH_ATTRIBUTES	0x06
#define PDU_SEARCH_ATTR_RSP	0x07

// A record's attributes, in the order of their IDs: each its ID (a 16-bit
// unsigned: 09) and its value. 35 nn: a sequence of nn bytes; 19: a 16-bit
// UUID; 0A: a 32-bit unsigned
struct TAttribute { u16 nId; u8 nBytes; u8 Value[20]; };

static const TAttribute Source[] =					// the sound's source (A2DP)
{
	{0x0000, 5, {0x0A, 0x00, 0x01, 0x00, 0x01}},			// the record's handle
	{0x0001, 5, {0x35, 3, 0x19, 0x11, 0x0A}},			// its class: AudioSource
	{0x0004, 18, {0x35, 16, 0x35, 6, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,	// L2CAP, PSM 0x19
			       0x35, 6, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03}},	// AVDTP 1.3
	{0x0005, 5, {0x35, 3, 0x19, 0x10, 0x02}},			// browsable: the public root
	{0x0009, 10, {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03}},	// the profile: A2DP 1.3
	{0x0311, 3, {0x09, 0x00, 0x01}},				// features: a player
};
static const TAttribute Target[] =					// what the speaker's buttons command (AVRCP)
{
	{0x0000, 5, {0x0A, 0x00, 0x01, 0x00, 0x02}},
	{0x0001, 5, {0x35, 3, 0x19, 0x11, 0x0C}},			// its class: A/V remote control target
	{0x0004, 18, {0x35, 16, 0x35, 6, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,	// L2CAP, PSM 0x17
			       0x35, 6, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04}},	// AVCTP 1.4
	{0x0005, 5, {0x35, 3, 0x19, 0x10, 0x02}},
	{0x0009, 10, {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0E, 0x09, 0x01, 0x04}},	// the profile: AVRCP 1.4
	{0x0311, 3, {0x09, 0x00, 0x01}},				// features: category 1, a player
};
static const TAttribute Controller[] =					// what sets the speaker's volume (AVRCP)
{
	{0x0000, 5, {0x0A, 0x00, 0x01, 0x00, 0x03}},
	{0x0001, 8, {0x35, 6, 0x19, 0x11, 0x0E, 0x19, 0x11, 0x0F}},	// its classes: A/V remote control, its controller
	{0x0004, 18, {0x35, 16, 0x35, 6, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,
			       0x35, 6, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04}},
	{0x0005, 5, {0x35, 3, 0x19, 0x10, 0x02}},
	{0x0009, 10, {0x35, 8, 0x35, 6, 0x19, 0x11, 0x0E, 0x09, 0x01, 0x04}},
	{0x0311, 3, {0x09, 0x00, 0x02}},				// features: category 2, an amplifier's
};

// the records, and the UUIDs in each (a search finds a record if all it asks
// for are among them)
#define RECORDS		3
static const struct { u32 nHandle; const TAttribute *pAttributes; unsigned nAttributes; u16 UUIDs[6]; } Records[RECORDS] =
{
	{0x00010001, Source, sizeof Source / sizeof Source[0], {0x110A, 0x0100, 0x0019, 0x1002, 0x110D}},
	{0x00010002, Target, sizeof Target / sizeof Target[0], {0x110C, 0x0100, 0x0017, 0x1002, 0x110E}},
	{0x00010003, Controller, sizeof Controller / sizeof Controller[0], {0x110E, 0x110F, 0x0100, 0x0017, 0x1002}},
};

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
static boolean Matches (unsigned nRecord, const u8 *p, unsigned n, unsigned *pUsed)
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
		for (unsigned k = 0; k < sizeof Records[0].UUIDs / sizeof Records[0].UUIDs[0]; k++)
		{
			bFound = bFound || (Records[nRecord].UUIDs[k] && nUUID == Records[nRecord].UUIDs[k]);
		}
		bAll = bAll && bFound;
		i += nS + nL;
	}
	return bAll;
}

// A record's attributes whose IDs an attribute ID list asks for (IDs, or
// ranges of them), as a sequence. \return its length; *pUsed: the list's
static unsigned Listed (unsigned nRecord, const u8 *p, unsigned n, u8 *pOut, unsigned *pUsed)
{
	const TAttribute *Attributes = Records[nRecord].pAttributes;
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
	for (unsigned a = 0; a < Records[nRecord].nAttributes; a++)
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
	if (nBytes < 5 || nMax < 320)
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
		unsigned nFound = 0;
		nOut = 4;
		for (unsigned r = 0; r < RECORDS; r++)
		{
			if (Matches (r, p, n, &nUsed))
			{
				pOut[nOut++] = (u8) (Records[r].nHandle >> 24);
				pOut[nOut++] = (u8) (Records[r].nHandle >> 16);
				pOut[nOut++] = (u8) (Records[r].nHandle >> 8);
				pOut[nOut++] = (u8) Records[r].nHandle;
				nFound++;
			}
		}
		nCode = PDU_SEARCH_RSP;
		pOut[0] = pOut[2] = 0;
		pOut[1] = pOut[3] = (u8) nFound;
		pOut[nOut++] = 0;			// (all of it: nothing to continue)
		break;
	}

	case PDU_ATTRIBUTES:				// a handle, how many bytes at most, the IDs: the attributes
	{
		unsigned r = RECORDS;
		if (n >= 6)
		{
			u32 nHandle = (u32) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
			for (r = 0; r < RECORDS && Records[r].nHandle != nHandle; r++)
			{
			}
		}
		if (r < RECORDS)
		{
			nCode = PDU_ATTRIBUTES_RSP;
			unsigned nList = Listed (r, p + 6, n - 6, pOut + 2, &nUsed);
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
	}

	case PDU_SEARCH_ATTRIBUTES:			// a pattern, how many bytes at most, the IDs: each record's attributes
	{
		nCode = PDU_SEARCH_ATTR_RSP;
		unsigned nLists = 0, nSkip;		// (three records' are 205 bytes: a one-byte length)
		for (unsigned r = 0; r < RECORDS; r++)
		{
			if (Matches (r, p, n, &nUsed) && nUsed + 2 <= n)
			{
				nLists += Listed (r, p + nUsed + 2, n - nUsed - 2, pOut + 4 + nLists, &nSkip);
			}
		}
		pOut[0] = 0;
		pOut[1] = (u8) (2 + nLists);
		pOut[2] = 0x35;
		pOut[3] = (u8) nLists;
		nOut = 4 + nLists;
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
