//
// bt_sdp.h
//
// The service records a speaker may ask for before it takes our stream: an
// A2DP audio source (AVDTP 1.3 on L2CAP PSM 0x19), and AVRCP 1.4's two (AVCTP
// on PSM 0x17): the target of the speaker's buttons, and the controller of
// its volume. The server's three requests are answered from them; a search
// for anything else finds nothing.
//
// Bluetooth Core specification, vol 3 part B.
//
#ifndef _gpu_bt_bt_sdp_h
#define _gpu_bt_bt_sdp_h

#include <circle/types.h>

/// \brief Answer an SDP request
/// \return The answer's length in pAnswer (at most nMax: 320 at least), 0 for none
unsigned SDPAnswer (const u8 *pRequest, unsigned nBytes, u8 *pAnswer, unsigned nMax);

#endif
