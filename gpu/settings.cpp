//
// settings.cpp
//
#include "settings.h"
#include <circle/string.h>
#include <circle/util.h>

CSettings::CSettings (void)
:	m_nCount (0),
	m_nBare (0),
	m_bComplete (TRUE)
{
}

// bLine: a command line (its "key=value"s end at any space), else a file of them (at a line's end)
void CSettings::Parse (const char *pText, unsigned nBytes, boolean bLine)
{
	m_nCount = 0;
	m_nBare = 0;
	m_bComplete = TRUE;
	const char *p = pText, *pEnd = pText + nBytes;
	while (p < pEnd)
	{
		const char *pLine = p;
		while (p < pEnd && *p != '\n' && !(bLine && (*p == ' ' || *p == '\t' || *p == '\r')))
		{
			p++;
		}
		const char *pLineEnd = p++;

		// the key: up to '=', spaces round it and the value dropped
		while (pLine < pLineEnd && (*pLine == ' ' || *pLine == '\t'))
		{
			pLine++;
		}
		if (pLine == pLineEnd || *pLine == '#')
		{
			continue;
		}
		const char *pEqual = pLine;
		while (pEqual < pLineEnd && *pEqual != '=')
		{
			pEqual++;
		}
		if (pEqual == pLineEnd)
		{
			// a command line's word without '=': kept as it is
			unsigned nWord = pLineEnd - pLine;
			if (!bLine)
			{
				continue;
			}
			if (m_nCount == MaxKeys || nWord >= KeyChars)
			{
				m_bComplete = FALSE;
				continue;
			}
			memcpy (m_Key[m_nCount], pLine, nWord);
			m_Key[m_nCount][nWord] = '\0';
			m_Value[m_nCount][0] = '\0';
			m_nBare |= 1U << m_nCount++;
			continue;
		}
		const char *pKeyEnd = pEqual, *pValue = pEqual + 1, *pValueEnd = pLineEnd;
		while (pKeyEnd > pLine && (pKeyEnd[-1] == ' ' || pKeyEnd[-1] == '\t'))
		{
			pKeyEnd--;
		}
		while (pValue < pValueEnd && (*pValue == ' ' || *pValue == '\t'))
		{
			pValue++;
		}
		while (pValueEnd > pValue && (pValueEnd[-1] == ' ' || pValueEnd[-1] == '\t' || pValueEnd[-1] == '\r'))
		{
			pValueEnd--;
		}
		unsigned nKey = pKeyEnd - pLine, nValue = pValueEnd - pValue;
		if (m_nCount == MaxKeys || nKey == 0 || nKey >= KeyChars || nValue >= ValueChars)
		{
			m_bComplete = FALSE;
			continue;
		}
		memcpy (m_Key[m_nCount], pLine, nKey);
		m_Key[m_nCount][nKey] = '\0';
		memcpy (m_Value[m_nCount], pValue, nValue);
		m_Value[m_nCount][nValue] = '\0';
		m_nCount++;
	}
}

const char *CSettings::Get (const char *pKey) const
{
	for (unsigned i = m_nCount; i-- > 0; )		// (the last one of a key)
	{
		if (strcmp (m_Key[i], pKey) == 0)
		{
			return m_Value[i];
		}
	}
	return nullptr;
}

void CSettings::Set (const char *pKey, const char *pValue)
{
	if (strlen (pKey) >= KeyChars || strlen (pValue) >= ValueChars)
	{
		return;
	}
	for (unsigned i = 0; i < m_nCount; i++)
	{
		if (strcmp (m_Key[i], pKey) == 0)
		{
			strcpy (m_Value[i], pValue);
			return;
		}
	}
	if (m_nCount < MaxKeys)
	{
		strcpy (m_Key[m_nCount], pKey);
		strcpy (m_Value[m_nCount], pValue);
		m_nCount++;
	}
}

void CSettings::Remove (const char *pKey)
{
	for (unsigned i = 0; i < m_nCount; i++)
	{
		if (strcmp (m_Key[i], pKey) == 0)
		{
			for (unsigned k = i; k + 1 < m_nCount; k++)
			{
				strcpy (m_Key[k], m_Key[k + 1]);
				strcpy (m_Value[k], m_Value[k + 1]);
			}
			u32 nLow = (1U << i) - 1;
			m_nBare = (m_nBare & nLow) | ((m_nBare >> 1) & ~nLow);
			m_nCount--;
			return;
		}
	}
}

unsigned CSettings::FormatLine (char *pBuffer, unsigned nSize) const
{
	CString Text;
	for (unsigned i = 0; i < m_nCount; i++)
	{
		Text.Append (i ? " " : "");
		Text.Append (m_Key[i]);
		if (!(m_nBare & (1U << i)))
		{
			Text.Append ("=");
			Text.Append (m_Value[i]);
		}
	}
	Text.Append ("\n");
	unsigned n = Text.GetLength () < nSize ? Text.GetLength () : nSize - 1;
	memcpy (pBuffer, (const char *) Text, n);
	pBuffer[n] = '\0';
	return n;
}

unsigned CSettings::Format (char *pBuffer, unsigned nSize) const
{
	static const struct { const char *pKey, *pComment; } Known[] =
	{
		{"volume", "# the sound's volume on HDMI, percent\n"},
		{"brightness", "# the panel's backlight, percent (its LED pin on GPIO12, pin 32)\n"},
		{"touchcal", "# the touch screen's calibration: the readings at the left, right, top and\n"
			     "# bottom edges, and 1: x from the controller's Y (Settings: Calibrate)\n"},
		{"mute", "# the sound muted: on, off\n"},
		{"bluetooth", "# Bluetooth: on, off (the speakers paired with: speakers.txt)\n"},
	};
	CString Text ("# piegpu: the user's settings (the installer page leaves this file alone);\n"
		      "# a key here wins over cmdline.txt's. Settings (a long press on the panel)\n"
		      "# writes it too\n");
	for (unsigned i = 0; i < m_nCount; i++)
	{
		for (unsigned k = 0; k < sizeof Known / sizeof Known[0]; k++)
		{
			if (strcmp (Known[k].pKey, m_Key[i]) == 0)
			{
				Text.Append (Known[k].pComment);
			}
		}
		Text.Append (m_Key[i]);
		Text.Append ("=");
		Text.Append (m_Value[i]);
		Text.Append ("\n");
	}
	unsigned n = Text.GetLength () < nSize ? Text.GetLength () : nSize - 1;
	memcpy (pBuffer, (const char *) Text, n);
	pBuffer[n] = '\0';
	return n;
}
