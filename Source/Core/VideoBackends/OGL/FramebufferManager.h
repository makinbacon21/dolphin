// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#ifdef HAVE_OCULUSSDK
#include "Kernel/OVR_Types.h"
#include "OVR_CAPI.h"
#include "OVR_CAPI_GL.h"
#include "Kernel/OVR_Math.h"
#endif

#include "VideoBackends/OGL/GLUtil.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"

#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/VR.h"

// On the GameCube, the game sends a request for the graphics processor to
// transfer its internal EFB (Embedded Framebuffer) to an area in GameCube RAM
// called the XFB (External Framebuffer). The size and location of the XFB is
// decided at the time of the copy, and the format is always YUYV. The video
// interface is given a pointer to the XFB, which will be decoded and
// displayed on the TV.
//
// There are two ways for Dolphin to emulate this:
//
// Real XFB mode:
//
// Dolphin will behave like the GameCube and encode the EFB to
// a portion of GameCube RAM. The emulated video interface will decode the data
// for output to the screen.
//
// Advantages: Behaves exactly like the GameCube.
// Disadvantages: Resolution will be limited.
//
// Virtual XFB mode:
//
// When a request is made to copy the EFB to an XFB, Dolphin
// will remember the RAM location and size of the XFB in a Virtual XFB list.
// The video interface will look up the XFB in the list and use the enhanced
// data stored there, if available.
//
// Advantages: Enables high resolution graphics, better than real hardware.
// Disadvantages: If the GameCube CPU writes directly to the XFB (which is
// possible but uncommon), the Virtual XFB will not capture this information.

// There may be multiple XFBs in GameCube RAM. This is the maximum number to
// virtualize.

namespace OGL {

struct XFBSource : public XFBSourceBase
{
	XFBSource(GLuint tex) : texture(tex) {}
	~XFBSource();

	void CopyEFB(float Gamma) override;
	void DecodeToTexture(u32 xfbAddr, u32 fbWidth, u32 fbHeight) override;
	void Draw(const MathUtil::Rectangle<int> &sourcerc,
		const MathUtil::Rectangle<float> &drawrc) const override;

	const GLuint texture;
};

class FramebufferManager : public FramebufferManagerBase
{
public:
	FramebufferManager(int targetWidth, int targetHeight, int msaaSamples);
	~FramebufferManager();

	// To get the EFB in texture form, these functions may have to transfer
	// the EFB to a resolved texture first.
	static GLuint GetEFBColorTexture(const EFBRectangle& sourceRc, int eye);
	static GLuint GetEFBDepthTexture(const EFBRectangle& sourceRc, int eye);

	static GLuint GetEFBFramebuffer(int eye) { return m_efbFramebuffer[eye]; }
	static GLuint GetXFBFramebuffer() { return m_xfbFramebuffer; }

	// Resolved framebuffer is only used in MSAA mode.
	static GLuint GetResolvedFramebuffer(int eye) { return m_resolvedFramebuffer[eye]; }

	static void SetFramebuffer(GLuint fb);

	static void RenderToEye(int eye);
	static void SwapRenderEye();

	// If in MSAA mode, this will perform a resolve of the specified rectangle, and return the resolve target as a texture ID.
	// Thus, this call may be expensive. Don't repeat it unnecessarily.
	// If not in MSAA mode, will just return the render target texture ID.
	// After calling this, before you render anything else, you MUST bind the framebuffer you want to draw to.
	static GLuint ResolveAndGetRenderTarget(const EFBRectangle &rect, int eye);

	// Same as above but for the depth Target.
	// After calling this, before you render anything else, you MUST bind the framebuffer you want to draw to.
	static GLuint ResolveAndGetDepthTarget(const EFBRectangle &rect, int eye);

	// Convert EFB content on pixel format change.
	// convtype=0 -> rgb8->rgba6, convtype=2 -> rgba6->rgb8
	static void ReinterpretPixelData(unsigned int convtype, int eye);

	// Oculus Rift
#ifdef HAVE_OCULUSSDK
	static ovrGLTexture m_eye_texture[2];
#endif
	static bool m_stereo3d;
	static int m_eye_count, m_current_eye;

private:
	XFBSourceBase* CreateXFBSource(unsigned int target_width, unsigned int target_height) override;
	void GetTargetSize(unsigned int *width, unsigned int *height, const EFBRectangle& sourceRc) override;

	void CopyToRealXFB(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc,float Gamma) override;

	static int m_targetWidth;
	static int m_targetHeight;
	static int m_msaaSamples;

	static GLenum m_textureType;

	static GLuint m_efbFramebuffer[2];
	static GLuint m_xfbFramebuffer;
	static GLuint m_efbColor[2];
	static GLuint m_efbDepth[2];
	static GLuint m_efbColorSwap[2];// will be hot swapped with m_efbColor when reinterpreting EFB pixel formats

	// Only used in MSAA mode, TODO: try to avoid them
	static GLuint m_resolvedFramebuffer[2];
	static GLuint m_resolvedColorTexture[2];
	static GLuint m_resolvedDepthTexture[2];

	// For pixel format draw
	static SHADER m_pixel_format_shaders[2];
};

}  // namespace OGL
