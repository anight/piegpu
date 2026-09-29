//
// v3dcheck.h
//
// A check of a V3D job's control lists before it runs, in the spirit of
// Mesa's vc4 kernel validator (vc4_validate.c): a wrong address or size in
// a list makes the V3D write over other memory or hang, silently. Checked:
//  - every packet one this renderer emits (others are unexpected), whole,
//    inside the list;
//  - the lists' shape: the binning mode, START_TILE_BINNING, then draws,
//    FLUSH last; the rendering mode first, tile coordinates inside the frame,
//    one end-of-frame store, last;
//  - every address the V3D reads or writes, with its extent, inside one
//    block of memory registered as the V3D's (CV3D::AddRegion): the render
//    target and the tile loads and stores (raster, T or LT extents), the
//    binner's tile memory and state, sublists, shader records, shader code,
//    uniform streams, index buffers, and vertex attributes over the indices a
//    draw really uses (read from the index buffer).
// Not checked: texture addresses (inside the uniform streams, where only the
// shader code says which words they are).
//
// A job that fails isn't run: V3DCheckJob () logs why (the first ones, then
// every 1000th) and returns FALSE.
//
#ifndef _drivers_v3dcheck_h
#define _drivers_v3dcheck_h

#include <circle/types.h>

/// \brief A CV3D::TJobCheck (CV3D::SetJobCheck)
/// \param nBinStart 0: a render-only job (CV3D::RunRender)
boolean V3DCheckJob (u32 nBinStart, u32 nBinEnd, u32 nRenderStart, u32 nRenderEnd);

/// \return Jobs checked and jobs refused so far
void V3DCheckStats (unsigned *pChecked, unsigned *pRefused);

#endif
