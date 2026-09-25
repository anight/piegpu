//
// v3d.h
//
// Bare-metal VideoCore IV 3D (V3D) access for the BCM2835:
// power-up via the firmware mailbox, control list building, job submission.
//
// All memory the V3D accesses is normal ARM memory (Circle heap). The GPU
// reaches it through the L2-cached bus alias (BUS_ADDRESS), which is coherent
// with the ARM behind its L1 data cache, so only L1 maintenance is needed.
//
#ifndef _v3d_h
#define _v3d_h

#include <circle/types.h>

class CV3D
{
public:
	CV3D (void);
	~CV3D (void);

	/// \brief Power up the V3D and check its identification
	boolean Initialize (void);

	/// \brief Allocate memory accessible by the V3D
	static void *Alloc (size_t nSize, size_t nAlign = 4096);

	/// \return Bus address of ARM memory for the V3D
	static u32 BusAddress (const void *p);

	/// \brief Write back ARM L1 cache lines of a range, so the V3D sees the data
	static void Flush (const void *p, size_t nSize);

	/// \brief Run a binning and a rendering control list, wait for completion
	/// \param pBinUs Microseconds spent binning (may be nullptr)
	/// \param pRenderUs Microseconds spent rendering (may be nullptr)
	/// \return Operation successful (FALSE on timeout or error)
	boolean RunJob (u32 nBinStart, u32 nBinEnd, u32 nRenderStart, u32 nRenderEnd,
			u32 nOverflowAddress, u32 nOverflowSize,
			unsigned *pBinUs = nullptr, unsigned *pRenderUs = nullptr);

	void DumpStatus (void);

private:
	static u32 Read (unsigned nOffset);
	static void Write (unsigned nOffset, u32 nValue);
};

/// Helper to build a control list in V3D accessible memory
class CControlList
{
public:
	CControlList (void *pBuffer, size_t nSize)
	:	m_pStart ((u8 *) pBuffer), m_pNext ((u8 *) pBuffer), m_pEnd ((u8 *) pBuffer + nSize) {}

	void Add8 (u8 uchValue)		{ *m_pNext++ = uchValue; }
	void Add16 (u16 usValue)	{ Add8 (usValue & 0xFF); Add8 (usValue >> 8); }
	void Add32 (u32 nValue)		{ Add16 (nValue & 0xFFFF); Add16 (nValue >> 16); }
	void AddFloat (float fValue)	{ union { float f; u32 u; } v; v.f = fValue; Add32 (v.u); }

	u32 GetStartBus (void) const	{ return CV3D::BusAddress (m_pStart); }
	u32 GetEndBus (void) const	{ return CV3D::BusAddress (m_pNext); }
	size_t GetSize (void) const	{ return m_pNext - m_pStart; }
	boolean Overflow (void) const	{ return m_pNext > m_pEnd; }

	void Flush (void) const		{ CV3D::Flush (m_pStart, GetSize ()); }

private:
	u8 *m_pStart;
	u8 *m_pNext;
	u8 *m_pEnd;
};

// Control list opcodes
#define V3D_HALT			0
#define V3D_NOP				1
#define V3D_FLUSH			4
#define V3D_START_TILE_BINNING		6
#define V3D_BRANCH_TO_SUBLIST		17
#define V3D_STORE_MS_TILE_BUFFER	24
#define V3D_STORE_MS_TILE_BUFFER_EOF	25
#define V3D_STORE_TILE_BUFFER_GENERAL	28
#define V3D_LOAD_TILE_BUFFER_GENERAL	29
#define V3D_INDEXED_PRIMITIVE_LIST	32
#define V3D_VERTEX_ARRAY_PRIMITIVES	33
#define V3D_GL_SHADER_STATE		64
#define V3D_NV_SHADER_STATE		65
#define V3D_CONFIGURATION_BITS		96
#define V3D_CLIP_WINDOW			102
#define V3D_VIEWPORT_OFFSET		103
#define V3D_CLIPPER_XY_SCALING		105
#define V3D_CLIPPER_Z_SCALE_OFFSET	106
#define V3D_TILE_BINNING_MODE_CONFIG	112
#define V3D_TILE_RENDERING_MODE_CONFIG	113
#define V3D_CLEAR_COLORS		114
#define V3D_TILE_COORDINATES		115

#define V3D_PRIM_TRIANGLES		4

#define V3D_TILE_SIZE			64
#define V3D_TILE_ALLOC_BLOCK		32	// initial tile allocation block size
#define V3D_TILE_STATE_SIZE		48	// per tile

#endif
