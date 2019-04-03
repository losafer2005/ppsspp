
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#include "gfx_es2/gpu_features.h"
#include <glsym/rglgen.h>
#include "libretro/LibretroGLContext.h"

bool LibretroGLContext::Init() {
	if (!LibretroHWRenderContext::Init(true))
		return false;

	g_Config.iGPUBackend = (int)GPUBackend::OPENGL;
	return true;
}

extern const struct rglgen_sym_map rglgen_symbol_map_ppsspp;
void LibretroGLContext::CreateDrawContext() {
	if (!glewInitDone) {
#if defined(HAVE_LIBNX)
		/*gl = HostGL_CreateGLInterface();
		gl->SetMode(MODE_OPENGL);

		if(!gl->Create(0, false, false)){
			ERROR_LOG(G3D, "EGL failed.\n");
			return;
		}
		
		gl->MakeCurrent();
		if (gl->GetMode() == GLInterfaceMode::MODE_OPENGL)
			SetGLCoreContext(true);

		SetGLCoreContext(true);
		*/
		//if (glewInit() != GLEW_OK) {
		//	ERROR_LOG(G3D, "glewInit() failed.\n");
		//	return;
		//}
#endif
		rglgen_resolve_symbols_custom(&eglGetProcAddress, &rglgen_symbol_map_ppsspp);
		glewInitDone = true;
		
		CheckGLExtensions();
	}
	draw_ = Draw::T3DCreateGLContext();
	renderManager_ = (GLRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
}

void LibretroGLContext::DestroyDrawContext() {
	LibretroHWRenderContext::DestroyDrawContext();
	renderManager_ = nullptr;
}
