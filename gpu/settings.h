//
// settings.h
//
// The user's settings: settings.txt on the card, a "key=value" a line ("#"
// starts a comment), read at boot. A key there wins over the kernel command
// line's (cmdline.txt, which the installer page writes and rewrites; it never
// writes settings.txt): volume=, brightness=, touchcal=.
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
	void Parse (const char *pText, unsigned nBytes);

	/// \return The key's value, or nullptr
	const char *Get (const char *pKey) const;

	/// \brief The key's value from now (added if it's new)
	void Set (const char *pKey, const char *pValue);

	/// \brief The file's text again (the known keys with what they are)
	/// \return Its length (at most nSize - 1, '\0' after it)
	unsigned Format (char *pBuffer, unsigned nSize) const;

	unsigned GetCount (void) const		{ return m_nCount; }
	const char *GetKey (unsigned i) const	{ return m_Key[i]; }

private:
	static const unsigned MaxKeys = 16;
	static const unsigned KeyChars = 32, ValueChars = 64;

	unsigned m_nCount;
	char m_Key[MaxKeys][KeyChars];
	char m_Value[MaxKeys][ValueChars];
};

#endif
