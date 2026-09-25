/*
 * tcuPglPlatform.cpp - the dEQP platform for pgl (pico-gpu): an OpenGL ES 2.0
 * context whose calls go through pgl on the PC to the Zero (the commands of
 * docs/protocol.md over the Zero's USB, pico/gpulink/host). The default
 * framebuffer is the panel: 320x240 RGB565, 24-bit depth, 8-bit stencil.
 */
#include "tcuPlatform.hpp"
#include "tcuRenderTarget.hpp"
#include "tcuDefs.hpp"
#include "gluPlatform.hpp"
#include "gluContextFactory.hpp"
#include "gluRenderContext.hpp"
#include "gluRenderConfig.hpp"
#include "glwFunctions.hpp"
#include "glwFunctionLoader.hpp"
#include "glwInitFunctions.hpp"
#include <cstring>

extern "C"
{
#include "pgpu.h"
#include "pgl.h"
}

namespace pgltarget
{

/* gl* name -> pgl function (generated from pgl.h by build-deqp.sh) */
static const struct
{
	const char *name;
	glw::GenericFuncType function;
} functions[] =
{
#include "pglFunctions.inl"
};

class Loader : public glw::FunctionLoader
{
public:
	glw::GenericFuncType get (const char *name) const override
	{
		for (const auto &f : functions)
		{
			if (std::strcmp (f.name, name) == 0)
			{
				return f.function;
			}
		}
		return nullptr;
	}
};

class RenderContext : public glu::RenderContext
{
public:
	RenderContext (const glu::RenderConfig &config)
	:	m_renderTarget (320, 240, tcu::PixelFormat (5, 6, 5, 0), 24, 8, 0)
	{
		if (config.type.getAPI () != glu::ApiType::es (2, 0))
		{
			throw tcu::NotSupportedError ("pgl: OpenGL ES 2.0 only");
		}
		if (config.surfaceType != glu::RenderConfig::SURFACETYPE_DONT_CARE
		    && config.surfaceType != glu::RenderConfig::SURFACETYPE_WINDOW)
		{
			throw tcu::NotSupportedError ("pgl: the panel is the only surface");
		}

		static bool linkUp = false;
		if (!linkUp)
		{
			pgpu_init ();			/* the USB link to the Zero */
			linkUp = true;
		}
		if (!pglInit ())
		{
			throw tcu::ResourceError ("pgl: no answer from the Zero");
		}

		Loader loader;
		glw::initES20 (&m_functions, &loader);
	}

	glu::ContextType getType (void) const override
	{
		return glu::ContextType (glu::ApiType::es (2, 0));
	}

	const glw::Functions &getFunctions (void) const override
	{
		return m_functions;
	}

	const tcu::RenderTarget &getRenderTarget (void) const override
	{
		return m_renderTarget;
	}

	void postIterate (void) override
	{
		pglSwapBuffers ();
	}

private:
	glw::Functions m_functions;
	tcu::RenderTarget m_renderTarget;
};

class ContextFactory : public glu::ContextFactory
{
public:
	ContextFactory (void)
	:	glu::ContextFactory ("pgl", "pico-gpu: pgl on the PC, the Zero over USB")
	{
	}

	glu::RenderContext *createContext (const glu::RenderConfig &config, const tcu::CommandLine &,
					   const glu::RenderContext *) const override
	{
		return new RenderContext (config);
	}
};

class Platform : public tcu::Platform, private glu::Platform
{
public:
	Platform (void)
	{
		m_contextFactoryRegistry.registerFactory (new ContextFactory ());
	}

	const glu::Platform &getGLPlatform (void) const override
	{
		return static_cast<const glu::Platform &> (*this);
	}
};

} // namespace pgltarget

tcu::Platform *createPlatform (void)
{
	return new pgltarget::Platform ();
}
