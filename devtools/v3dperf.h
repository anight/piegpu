//
// v3dperf.h
//
// VideoCore IV V3D performance counters. Works no matter who drives the V3D
// (firmware OpenGL ES or bare-metal code), since the counters are hardware.
//
#ifndef _devtools_v3dperf_h
#define _devtools_v3dperf_h

#include <circle/types.h>

class CV3DPerf
{
public:
	static const unsigned Counters = 16;

public:
	CV3DPerf (void);

	/// \brief Select sources, clear and enable the counters
	void Start (void);

	/// \brief Stop the counters and log the results
	/// \param pTitle Description of the measured workload
	/// \param nUs Wall time of the measured workload
	/// \param nPixels Number of pixels rendered (all frames)
	void StopAndLog (const char *pTitle, unsigned nUs, unsigned nPixels);

private:
	u32 m_Values[Counters];
};

#endif
