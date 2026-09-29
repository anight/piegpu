//
// build_info.cpp
//
// Compiled at every build of the gpu app (gpu/Makefile): the Makefile
// defines PGPU_VERSION, PGPU_BUILT and PGPU_CIRCLE.
//
#include "build_info.h"

#define STR2(x)	#x
#define STR(x)	STR2 (x)

const char g_BuildInfo[] __attribute__ ((used)) =
	BUILD_INFO_START
	"fw=" PGPU_VERSION " fwbuilt=" PGPU_BUILT
	" fwconfig=RASPPI=" STR (RASPPI) ",AArch" STR (AARCH) ",Circle_" PGPU_CIRCLE
	",GCC_" STR (__GNUC__) "." STR (__GNUC_MINOR__) "." STR (__GNUC_PATCHLEVEL__)
	"\x03";

const char *GetBuildInfo (void)
{
	static char Line[sizeof g_BuildInfo];
	if (!Line[0])
	{
		const char *p = g_BuildInfo + sizeof BUILD_INFO_START - 1;
		unsigned i = 0;
		while (p[i] != BUILD_INFO_END)
		{
			Line[i] = p[i];
			i++;
		}
		Line[i] = '\0';
	}

	return Line;
}
