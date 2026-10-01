//
// settings.h
//
// The user's settings: settings.txt on the card, a "key=value" a line ("#"
// starts a comment), read at boot. A key there wins over the kernel command
// line's (cmdline.txt, which the installer page writes and rewrites; it never
// writes settings.txt): volume=, mute=, brightness=, touchcal=, bluetooth=.
//
// The kernel command line too (cmdline.txt: its options apart by spaces),
// for Settings to change its options (ParseLine, FormatLine).
//
#ifndef _gpu_settings_h
#define _gpu_settings_h

#include <circle/types.h>

class CSettings
{
public:
	static const unsigned MaxBytes = 2048;		// the file's, at most

	CSettings (void);

	/// \brief Take the file's text (nBytes of it)
	void Parse (const char *pText, unsigned nBytes)		{ Parse (pText, nBytes, FALSE); }
	/// \brief Take a command line's: "key=value"s apart by spaces
	void ParseLine (const char *pText, unsigned nBytes)	{ Parse (pText, nBytes, TRUE); }
	/// \return FALSE if some of the text didn't fit (too many keys, or one too
	///	    long): what Format gives back then isn't all of it
	boolean IsComplete (void) const				{ return m_bComplete; }

	/// \return The key's value, or nullptr
	const char *Get (const char *pKey) const;

	/// \brief The key's value from now (added if it's new)
	void Set (const char *pKey, const char *pValue);
	/// \brief The key gone
	void Remove (const char *pKey);

	/// \brief The file's text again (the known keys with what they are)
	/// \return Its length (at most nSize - 1, '\0' after it)
	unsigned Format (char *pBuffer, unsigned nSize) const;
	/// \brief The command line's text again: its options apart by spaces, a line
	unsigned FormatLine (char *pBuffer, unsigned nSize) const;

	unsigned GetCount (void) const		{ return m_nCount; }
	const char *GetKey (unsigned i) const	{ return m_Key[i]; }

private:
	void Parse (const char *pText, unsigned nBytes, boolean bLine);

	static const unsigned MaxKeys = 24;
	static const unsigned KeyChars = 32, ValueChars = 64;

	unsigned m_nCount;
	u32 m_nBare;			// a command line's words without '=' (a bit each)
	boolean m_bComplete;
	char m_Key[MaxKeys][KeyChars];
	char m_Value[MaxKeys][ValueChars];
};

#endif
