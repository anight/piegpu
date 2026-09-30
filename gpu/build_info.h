//
// build_info.h
//
// This build of the gpu app: its version, build time and configuration, as
// one line kept in kernel.img, where the installer finds it on a card
// (gpu/install: PGI INFO) and the page shows it:
//
//   fw=<major.minor.patch.build> fwgit=<git describe> fwbuilt=<UTC, ISO 8601>
//   fwconfig=RASPPI=1,AArch32,Circle_Step51.1,GCC_14.2.0
//
// The version: VERSION's major.minor.patch (the project's, by hand) and the
// build number (every build the next, devtools/next-build.sh). fw= comes
// first: the installer finds the line on a card by it (gpu/install).
//
// In kernel.img it's framed by BUILD_INFO_START and BUILD_INFO_END.
//
#ifndef _gpu_build_info_h
#define _gpu_build_info_h

#define BUILD_INFO_START	"\x01PGPU-BUILD\x02"
#define BUILD_INFO_END		'\x03'

extern const char g_BuildInfo[];	// with the framing

// the line itself (without the framing)
const char *GetBuildInfo (void);

// the version alone ("major.minor.patch.build"), the commit ("<git
// describe>"), the build time (UTC, ISO 8601)
const char *GetBuildVersion (void);
const char *GetBuildGit (void);
const char *GetBuildTime (void);

#endif
