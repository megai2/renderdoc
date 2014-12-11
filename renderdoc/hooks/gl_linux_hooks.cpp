/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2014 Crytek
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <dlfcn.h>
#include <stdio.h>

#include "hooks/hooks.h"

#include "driver/gl/gl_common.h"
#include "driver/gl/gl_hookset.h"
#include "driver/gl/gl_driver.h"

#include "common/threading.h"
#include "common/string_utils.h"

// bit of a hack
namespace Keyboard { extern Display *CurrentXDisplay; }

typedef GLXContext (*PFNGLXCREATECONTEXTPROC)(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct);
typedef void (*PFNGLXDESTROYCONTEXTPROC)(Display *dpy, GLXContext ctx);
typedef const char *(*PFNGLXQUERYEXTENSIONSSTRING)(Display *dpy, int screen);
typedef Bool (*PFNGLXMAKECURRENTPROC)(Display *dpy, GLXDrawable drawable, GLXContext ctx);
typedef void (*PFNGLXSWAPBUFFERSPROC)(Display *dpy, GLXDrawable drawable);
typedef XVisualInfo* (*PFNGLXGETVISUALFROMFBCONFIGPROC)(Display *dpy, GLXFBConfig config);
typedef int (*PFNGLXGETCONFIGPROC)(Display *dpy, XVisualInfo *vis, int attrib, int * value);
typedef Bool (*PFNGLXQUERYEXTENSIONPROC)(Display *dpy, int *errorBase, int *eventBase);

void *libGLdlsymHandle = RTLD_NEXT; // default to RTLD_NEXT, but overwritten if app calls dlopen() on real libGL

#define HookInit(function) \
	if(!strcmp(func, STRINGIZE(function))) \
	{ \
		OpenGLHook::glhooks.GL.function = (CONCAT(function, _hooktype))realFunc; \
		return (__GLXextFuncPtr)&CONCAT(function, _renderdoc_hooked); \
	}

#define HookExtension(funcPtrType, function) \
	if(!strcmp(func, STRINGIZE(function))) \
	{ \
		OpenGLHook::glhooks.GL.function = (funcPtrType)realFunc; \
		return (__GLXextFuncPtr)&CONCAT(function, _renderdoc_hooked); \
	}

#define HookExtensionAlias(funcPtrType, function, alias) \
	if(!strcmp(func, STRINGIZE(alias))) \
	{ \
		OpenGLHook::glhooks.GL.function = (funcPtrType)realFunc; \
		return (__GLXextFuncPtr)&CONCAT(function, _renderdoc_hooked); \
	}

/*
	in bash:

    function HookWrapper()
    {
        N=$1;
        echo -n "#define HookWrapper$N(ret, function";
            for I in `seq 1 $N`; do echo -n ", t$I, p$I"; done;
        echo ") \\";

        echo -en "\ttypedef ret (*CONCAT(function, _hooktype)) (";
            for I in `seq 1 $N`; do echo -n "t$I"; if [ $I -ne $N ]; then echo -n ", "; fi; done;
        echo "); \\";

        echo -e "\textern \"C\" __attribute__ ((visibility (\"default\"))) \\";

        echo -en "\tret function(";
            for I in `seq 1 $N`; do echo -n "t$I p$I"; if [ $I -ne $N ]; then echo -n ", "; fi; done;
        echo ") \\";
 
        echo -en "\t{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(";
            for I in `seq 1 $N`; do echo -n "p$I"; if [ $I -ne $N ]; then echo -n ", "; fi; done;
        echo "); } \\";

        echo -en "\tret CONCAT(function,_renderdoc_hooked)(";
            for I in `seq 1 $N`; do echo -n "t$I p$I"; if [ $I -ne $N ]; then echo -n ", "; fi; done;
        echo ") \\";
 
        echo -en "\t{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(";
            for I in `seq 1 $N`; do echo -n "p$I"; if [ $I -ne $N ]; then echo -n ", "; fi; done;
        echo -n "); }";
    }

	for I in `seq 0 ...`; do HookWrapper $I; echo; done

	*/

// don't want these definitions, the only place we'll use these is as parameter/variable names
#ifdef near
#undef near
#endif

#ifdef far
#undef far
#endif

// the _renderdoc_hooked variants are to make sure we always have a function symbol
// exported that we can return from glXGetProcAddress. If another library (or the app)
// creates a symbol called 'glEnable' we'll return the address of that, and break
// badly. Instead we leave the 'naked' versions for applications trying to import those
// symbols, and declare the _renderdoc_hooked for returning as a func pointer.

#define HookWrapper0(ret, function) \
	typedef ret (*CONCAT(function, _hooktype)) (); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function() \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(); } \
	ret CONCAT(function,_renderdoc_hooked)() \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(); }
#define HookWrapper1(ret, function, t1, p1) \
	typedef ret (*CONCAT(function, _hooktype)) (t1); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1); }
#define HookWrapper2(ret, function, t1, p1, t2, p2) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2); }
#define HookWrapper3(ret, function, t1, p1, t2, p2, t3, p3) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3); }
#define HookWrapper4(ret, function, t1, p1, t2, p2, t3, p3, t4, p4) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4); }
#define HookWrapper5(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5); }
#define HookWrapper6(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6); }
#define HookWrapper7(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7); }
#define HookWrapper8(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8); }
#define HookWrapper9(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9); }
#define HookWrapper10(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10); }
#define HookWrapper11(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10, t11, p11) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11); }
#define HookWrapper12(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10, t11, p11, t12, p12) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12); }
#define HookWrapper13(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10, t11, p11, t12, p12, t13, p13) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13); }
#define HookWrapper14(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10, t11, p11, t12, p12, t13, p13, t14, p14) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14); }
#define HookWrapper15(ret, function, t1, p1, t2, p2, t3, p3, t4, p4, t5, p5, t6, p6, t7, p7, t8, p8, t9, p9, t10, p10, t11, p11, t12, p12, t13, p13, t14, p14, t15, p15) \
	typedef ret (*CONCAT(function, _hooktype)) (t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15); \
	extern "C" __attribute__ ((visibility ("default"))) \
	ret function(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15); } \
	ret CONCAT(function,_renderdoc_hooked)(t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15) \
	{ SCOPED_LOCK(glLock); return OpenGLHook::glhooks.GetDriver()->function(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15); }

Threading::CriticalSection glLock;

class OpenGLHook : LibraryHook
{
	public:
		OpenGLHook()
		{
			LibraryHooks::GetInstance().RegisterHook("libGL.so", this);
			
			RDCEraseEl(GL);

			// TODO: need to check against implementation to ensure we don't claim to support
			// an extension that it doesn't!

			glXExts.push_back("GLX_ARB_extensions_string");
			//glXExts.push_back("GLX_ARB_multisample");
			glXExts.push_back("GLX_ARB_create_context");
			glXExts.push_back("GLX_ARB_create_context_profile");
			
			merge(glXExts, glXExtsString, ' ');

			m_GLDriver = NULL;

			m_EnabledHooks = true;
			m_PopulatedHooks = false;
		}
		~OpenGLHook()
		{
			delete m_GLDriver;
		}

		bool CreateHooks(const char *libName)
		{
			if(!m_EnabledHooks)
				return false;

			bool success = SetupHooks(GL);

			if(!success) return false;
			
			m_HasHooks = true;

			return true;
		}

		void EnableHooks(const char *libName, bool enable)
		{
			m_EnabledHooks = enable;
		}
		
		static OpenGLHook glhooks;

		const GLHookSet &GetRealFunctions()
		{
			if(!m_PopulatedHooks)
				m_PopulatedHooks = PopulateHooks();
			return GL;
		}

		void MakeContextCurrent(GLWindowingData data)
		{
			if(glXMakeCurrent_real)
				glXMakeCurrent_real(data.dpy, data.wnd, data.ctx);
		}
		
		GLWindowingData MakeContext(GLWindowingData share)
		{
			GLWindowingData ret = {0};
			if(glXCreateContextAttribsARB_real)
			{
				const int attribs[] = {
					GLX_CONTEXT_MAJOR_VERSION_ARB,
					3,
					GLX_CONTEXT_MINOR_VERSION_ARB,
					2,
					GLX_CONTEXT_FLAGS_ARB,
					0,
					0, 0,
				};

				PFNGLXCHOOSEFBCONFIGPROC glXChooseFBConfigProc = (PFNGLXCHOOSEFBCONFIGPROC)dlsym(RTLD_NEXT, "glXChooseFBConfig");

				if(glXChooseFBConfigProc)
				{
					// don't need to care about the fb config as we won't be using the default framebuffer (backbuffer)
					int visAttribs[] = { 0 };
					int numCfgs = 0;
					GLXFBConfig *fbcfg = glXChooseFBConfigProc(dpy, DefaultScreen(dpy), visAttribs, &numCfgs);

					if(fbcfg)
					{
						ret.dpy = share.dpy;
						ret.ctx = glXCreateContextAttribsARB_real(share.dpy, fbcfg[0], share.ctx, false, attribs);
					}
				}
			}
			return ret;
		}

		void DeleteContext(GLWindowingData context)
		{
			if(context.ctx && glXDestroyContext_real)
				glXDestroyContext_real(context.dpy, context.ctx);
		}

		WrappedOpenGL *GetDriver()
		{
			if(m_GLDriver == NULL)
				m_GLDriver = new WrappedOpenGL("", GL);

			return m_GLDriver;
		}

		PFNGLXCREATECONTEXTPROC glXCreateContext_real;
		PFNGLXDESTROYCONTEXTPROC glXDestroyContext_real;
		PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB_real;
		PFNGLXGETPROCADDRESSPROC glXGetProcAddress_real;
		PFNGLXMAKECURRENTPROC glXMakeCurrent_real;
		PFNGLXSWAPBUFFERSPROC glXSwapBuffers_real;
		PFNGLXGETCONFIGPROC glXGetConfig_real;
		PFNGLXGETVISUALFROMFBCONFIGPROC glXGetVisualFromFBConfig_real;
		PFNGLXQUERYEXTENSIONPROC glXQueryExtension_real;

		WrappedOpenGL *m_GLDriver;
		
		GLHookSet GL;
		
		vector<string> glXExts;
		string glXExtsString;

		set<GLXContext> m_Contexts;

		bool m_PopulatedHooks;
		bool m_HasHooks;
		bool m_EnabledHooks;

		bool SetupHooks(GLHookSet &GL);
		bool PopulateHooks();
};

DefineDLLExportHooks();
DefineGLExtensionHooks();

__attribute__ ((visibility ("default")))
GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct)
{
	GLXContext ret = OpenGLHook::glhooks.glXCreateContext_real(dpy, vis, shareList, direct);
	
	GLInitParams init;
	
	init.width = 0;
	init.height = 0;
	
	int value = 0;
	
	if(Keyboard::CurrentXDisplay == NULL) Keyboard::CurrentXDisplay = dpy;	

	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_BUFFER_SIZE, &value); init.colorBits = value;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_DEPTH_SIZE, &value); init.depthBits = value;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_STENCIL_SIZE, &value); init.stencilBits = value;
	value = 1; // default to srgb
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, &value); init.isSRGB = value;
	value = 1;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_SAMPLES_ARB, &value); init.isSRGB = RDCMAX(1, value);

	GLWindowingData data;
	data.dpy = dpy;
	data.wnd = (GLXDrawable)NULL;
	data.ctx = ret;

	OpenGLHook::glhooks.GetDriver()->CreateContext(data, shareList, init);

	return ret;
}

__attribute__ ((visibility ("default")))
void glXDestroyContext(Display *dpy, GLXContext ctx)
{
	OpenGLHook::glhooks.GetDriver()->DeleteContext(ctx);

	OpenGLHook::glhooks.glXDestroyContext_real(dpy, ctx);
}

__attribute__ ((visibility ("default")))
GLXContext glXCreateContextAttribsARB(Display *dpy, GLXFBConfig config, GLXContext shareList, Bool direct, const int *attribList)
{
	const int *attribs = attribList;
	vector<int> attribVec;

	if(RenderDoc::Inst().GetCaptureOptions().DebugDeviceMode)
	{
		bool flagsNext = false;
		bool flagsFound = false;
		const int *a = attribList;
		while(*a)
		{
			int val = *a;

			if(flagsNext)
			{
				val |= GLX_CONTEXT_DEBUG_BIT_ARB;
				flagsNext = false;
			}

			if(val == GLX_CONTEXT_FLAGS_ARB)
			{
				flagsNext = true;
				flagsFound = true;
			}

			attribVec.push_back(val);

			a++;
		}

		if(!flagsFound)
		{
			attribVec.push_back(GLX_CONTEXT_FLAGS_ARB);
			attribVec.push_back(GLX_CONTEXT_DEBUG_BIT_ARB);
		}

		attribVec.push_back(0);

		attribs = &attribVec[0];
	}

	GLXContext ret = OpenGLHook::glhooks.glXCreateContextAttribsARB_real(dpy, config, shareList, direct, attribs);

	XVisualInfo *vis = OpenGLHook::glhooks.glXGetVisualFromFBConfig_real(dpy, config);	
	
	GLInitParams init;
	
	init.width = 0;
	init.height = 0;
	
	int value = 0;
	
	if(Keyboard::CurrentXDisplay == NULL) Keyboard::CurrentXDisplay = dpy;
	
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_BUFFER_SIZE, &value); init.colorBits = value;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_DEPTH_SIZE, &value); init.depthBits = value;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_STENCIL_SIZE, &value); init.stencilBits = value;
	value = 1; // default to srgb
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, &value); init.isSRGB = value;
	value = 1;
	OpenGLHook::glhooks.glXGetConfig_real(dpy, vis, GLX_SAMPLES_ARB, &value); init.isSRGB = RDCMAX(1, value);

	XFree(vis);
	
	GLWindowingData data;
	data.dpy = dpy;
	data.wnd = (GLXDrawable)NULL;
	data.ctx = ret;

	OpenGLHook::glhooks.GetDriver()->CreateContext(data, shareList, init);

	return ret;
}

__attribute__ ((visibility ("default")))
Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
	Bool ret = OpenGLHook::glhooks.glXMakeCurrent_real(dpy, drawable, ctx);
	
	if(ctx && OpenGLHook::glhooks.m_Contexts.find(ctx) == OpenGLHook::glhooks.m_Contexts.end())
	{
		OpenGLHook::glhooks.m_Contexts.insert(ctx);

		OpenGLHook::glhooks.PopulateHooks();
	}

	GLWindowingData data;
	data.dpy = dpy;
	data.wnd = drawable;
	data.ctx = ctx;
	
	OpenGLHook::glhooks.GetDriver()->ActivateContext(data);

	return ret;
}

__attribute__ ((visibility ("default")))
void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
	Window root;
	int x, y;
	unsigned int width, height, border_width, depth;
	XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height, &border_width, &depth);	
	
	OpenGLHook::glhooks.GetDriver()->WindowSize((void *)drawable, width, height);
	
	OpenGLHook::glhooks.GetDriver()->Present((void *)drawable);

	OpenGLHook::glhooks.glXSwapBuffers_real(dpy, drawable);
}

__attribute__ ((visibility ("default")))
Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase)
{
	return OpenGLHook::glhooks.glXQueryExtension_real(dpy, errorBase, eventBase);
}

__attribute__ ((visibility ("default")))
const char *glXQueryExtensionsString(Display *dpy, int screen)
{
#if !defined(_RELEASE) && 0
	PFNGLXQUERYEXTENSIONSSTRING glXGetExtStr = (PFNGLXQUERYEXTENSIONSSTRING)dlsym(libGLdlsymHandle, "glXQueryExtensionsString");
	string realExtsString = glXGetExtStr(dpy, screen);
	vector<string> realExts;
	split(realExtsString, realExts, ' ');
#endif
	return OpenGLHook::glhooks.glXExtsString.c_str();
}

bool OpenGLHook::SetupHooks(GLHookSet &GL)
{
	bool success = true;
	
	if(glXGetProcAddress_real == NULL)          glXGetProcAddress_real = (PFNGLXGETPROCADDRESSPROC)dlsym(libGLdlsymHandle, "glXGetProcAddress");
	if(glXCreateContext_real == NULL)           glXCreateContext_real = (PFNGLXCREATECONTEXTPROC)dlsym(libGLdlsymHandle, "glXCreateContext");
	if(glXDestroyContext_real == NULL)           glXDestroyContext_real = (PFNGLXDESTROYCONTEXTPROC)dlsym(libGLdlsymHandle, "glXDestroyContext");
	if(glXCreateContextAttribsARB_real == NULL) glXCreateContextAttribsARB_real = (PFNGLXCREATECONTEXTATTRIBSARBPROC)dlsym(libGLdlsymHandle, "glXCreateContextAttribsARB");
	if(glXMakeCurrent_real == NULL)             glXMakeCurrent_real = (PFNGLXMAKECURRENTPROC)dlsym(libGLdlsymHandle, "glXMakeCurrent");
	if(glXSwapBuffers_real == NULL)             glXSwapBuffers_real = (PFNGLXSWAPBUFFERSPROC)dlsym(libGLdlsymHandle, "glXSwapBuffers");
	if(glXGetConfig_real == NULL)               glXGetConfig_real = (PFNGLXGETCONFIGPROC)dlsym(libGLdlsymHandle, "glXGetConfig");
	if(glXGetVisualFromFBConfig_real == NULL)   glXGetVisualFromFBConfig_real = (PFNGLXGETVISUALFROMFBCONFIGPROC)dlsym(libGLdlsymHandle, "glXGetVisualFromFBConfig");
	if(glXQueryExtension_real == NULL)               glXQueryExtension_real = (PFNGLXQUERYEXTENSIONPROC)dlsym(libGLdlsymHandle, "glXQueryExtension");

	return success;
}

__attribute__ ((visibility ("default")))
__GLXextFuncPtr glXGetProcAddress(const GLubyte *f)
{
	__GLXextFuncPtr realFunc = OpenGLHook::glhooks.glXGetProcAddress_real(f);
	const char *func = (const char *)f;

	// if the client code did dlopen on libGL then tried to fetch some functions
	// we don't hook/export it will fail, so allow these to pass through
	if(!strcmp(func, "glXChooseVisual") ||
		 !strcmp(func, "glXDestroyContext") ||
		 !strcmp(func, "glXChooseFBConfig") ||
		 !strcmp(func, "glXQueryDrawable"))
	{
		if(realFunc != NULL) return realFunc;

		if(libGLdlsymHandle != NULL)
			return (__GLXextFuncPtr)dlsym(libGLdlsymHandle, (const char *)f);
	}

	// handle a few functions that we only export as real functions, just
	// in case
	if(!strcmp(func, "glXCreateContext"))           return (__GLXextFuncPtr)&glXCreateContext;
	if(!strcmp(func, "glXDestroyContext"))          return (__GLXextFuncPtr)&glXDestroyContext;
	if(!strcmp(func, "glXCreateContextAttribsARB")) return (__GLXextFuncPtr)&glXCreateContextAttribsARB;
	if(!strcmp(func, "glXMakeCurrent"))             return (__GLXextFuncPtr)&glXMakeCurrent;
	if(!strcmp(func, "glXSwapBuffers"))             return (__GLXextFuncPtr)&glXSwapBuffers;
	if(!strcmp(func, "glXQueryExtension"))          return (__GLXextFuncPtr)&glXQueryExtension;
	if(!strcmp(func, "glXQueryExtensionsString"))   return (__GLXextFuncPtr)&glXQueryExtensionsString;

	// if the real RC doesn't support this function, don't bother hooking
	if(realFunc == NULL)
		return realFunc;
	
	DLLExportHooks();
	HookCheckGLExtensions();

#if 0
	// claim not to know this extension!
	RDCDEBUG("Claiming not to know extension that is available - %s", func);
#endif
	return NULL;
}

__attribute__ ((visibility ("default")))
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *f)
{
	return glXGetProcAddress(f);
}

typedef void* (*DLOPENPROC)(const char*,int);
DLOPENPROC realdlopen = NULL;

__attribute__ ((visibility ("default")))
void *dlopen(const char *filename, int flag)
{
	if(realdlopen == NULL) realdlopen = (DLOPENPROC)dlsym(RTLD_NEXT, "dlopen");

	void *ret = realdlopen(filename, flag);

	if(filename && ret && strstr(filename, "libGL.so"))
	{
		RDCDEBUG("Redirecting dlopen to ourselves");
		libGLdlsymHandle = ret;
		OpenGLHook::glhooks.CreateHooks("libGL.so");
		ret = realdlopen("librenderdoc.so", flag);
	}

	return ret;
}

bool OpenGLHook::PopulateHooks()
{
	bool success = true;

	if(glXGetProcAddress_real == NULL)
		glXGetProcAddress_real = (PFNGLXGETPROCADDRESSPROC)dlsym(libGLdlsymHandle, "glXGetProcAddress");

	glXGetProcAddress_real((const GLubyte *)"glXCreateContextAttribsARB");
	
#undef HookInit
#define HookInit(function) if(GL.function == NULL) { GL.function = (CONCAT(function, _hooktype))dlsym(libGLdlsymHandle, STRINGIZE(function)); glXGetProcAddress((const GLubyte *)STRINGIZE(function)); }
	
	// cheeky
#undef HookExtension
#define HookExtension(funcPtrType, function) glXGetProcAddress((const GLubyte *)STRINGIZE(function))
#undef HookExtensionAlias
#define HookExtensionAlias(funcPtrType, function, alias)

	DLLExportHooks();
	HookCheckGLExtensions();

	return true;
}

OpenGLHook OpenGLHook::glhooks;

const GLHookSet &GetRealFunctions() { return OpenGLHook::glhooks.GetRealFunctions(); }

void MakeContextCurrent(GLWindowingData data)
{
	OpenGLHook::glhooks.MakeContextCurrent(data);
}

GLWindowingData MakeContext(GLWindowingData share)
{
	return OpenGLHook::glhooks.MakeContext(share);
}

void DeleteContext(GLWindowingData context)
{
	OpenGLHook::glhooks.DeleteContext(context);
}

