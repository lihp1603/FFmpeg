/**
 * FFmpeg filter for applying GLSL transitions between video streams.
 *
 * @see https://gl-transitions.com/
 */

#include "libavutil/opt.h"
#include "internal.h"
#include "framesync.h"

#ifndef __APPLE__
# define GL_TRANSITION_USING_EGL //remove this line if you don't want to use EGL
//新增一种在x server下的方式
#define USING_EGL_EXT
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#endif

#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
# include <OpenGL/gl3.h>
#else
#ifdef USING_EGL_EXT
#include <GLES2/gl2.h>
#else
# include <GL/glew.h>
#endif
#endif

#ifdef GL_TRANSITION_USING_EGL
# include <EGL/egl.h>
#else
# include <GLFW/glfw3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define FROM (0)
#define TO   (1)

#define PIXEL_FORMAT (GL_RGB)

#ifdef GL_TRANSITION_USING_EGL
static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE};
#endif

//这里在一个gl中标准化的设备坐标中定义需要绘制的画布的顶点坐标值
static const float position[12] = {
    -1.0f, -1.0f,
  	 1.0f, -1.0f, 
  	-1.0f, 1.0f, 
  	-1.0f, 1.0f, 
  	 1.0f, -1.0f, 
  	 1.0f, 1.0f
};

//厉害的方法，一般我们做纹理贴图的时候，会将纹理贴图的坐标像顶点坐标一样，从cpu传入到gpu
//大家都知道,这种传输是效率比较低的方法
//这里,作者用了一种从顶点坐标转化的方法来实现纹理贴图坐标，从而提升效率
// vec2 uv = position * 0.5 + 0.5;这个地方是把顶点坐标从[-1,1]转化为[0,1]
//这里将[-1,1]转化为[0,1]主要是为了和后面的纹理贴图坐标,减少一次纹理贴图坐标的传输
//厉害

//_uv = vec2(uv.x, 1.0 - uv.y);
//相当于_uv.x=uv.x;_uv.y=1-uv.y
//将这个_uv代入到后面的函数getFromColor和getToColor函数中再进行一次计算
//变化为texture2D(form,_uv.x,1.0-(_uv.y))代入化简texture2D(form,_uv.x,_uv.y)
static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "varying vec2 _uv;\n"
  "void main(void) {\n"
  "  gl_Position = vec4(position, 0, 1);\n"
  "  vec2 uv = position * 0.5 + 0.5;\n"
  "  _uv = vec2(uv.x, 1.0 - uv.y);\n"
  "}\n";

static const GLchar *f_shader_template =
  "varying vec2 _uv;\n"
  "uniform sampler2D from;\n"
  "uniform sampler2D to;\n"
  "uniform float progress;\n"
  "uniform float ratio;\n"
  "uniform float _fromR;\n"
  "uniform float _toR;\n"
  "\n"
  "vec4 getFromColor(vec2 uv) {\n"
  "  return texture2D(from, vec2(uv.x, 1.0 - uv.y));\n"
  "}\n"
  "\n"
  "vec4 getToColor(vec2 uv) {\n"
  "  return texture2D(to, vec2(uv.x, 1.0 - uv.y));\n"
  "}\n"
  "\n"
  "\n%s\n"
  "void main() {\n"
  "  gl_FragColor = transition(_uv);\n"
  "}\n";


//GLSL中mix函数genType mix (genType x, genType y, genType a)是线性插值的实现方法,
//它返回线性混合的x和y，如：x*(1-a)+y*a
// default to a basic fade effect
static const GLchar *f_default_transition_source =
  "vec4 transition (vec2 uv) {\n"
  "  return mix(\n"
  "    getFromColor(uv),\n"
  "    getToColor(uv),\n"
  "    progress\n"
  "  );\n"
  "}\n";

#ifdef USING_EGL_EXT
typedef struct __tagOffEglExt{
 	int g_device_fd;
	uint32_t connector_id;
	drmModeModeInfo mode_info;
	drmModeCrtc *crtc;
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;
	struct gbm_bo *previous_bo;
	uint32_t previous_fb;
	EGLConfig *configs;
}OffEglExt;
#endif

typedef struct {
  const AVClass *class;
  FFFrameSync fs;

  // input options
  double duration;
  double offset;
  char *source;

  // timestamp of the first frame in the output, in the timebase units
  int64_t first_pts;

  // uniforms
  GLuint        from;
  GLuint        to;
  GLint         progress;
  GLint         ratio;
  GLint         _fromR;
  GLint         _toR;

  // internal state
  GLuint        posBuf;
  GLuint        program;
#ifdef GL_TRANSITION_USING_EGL
  EGLDisplay eglDpy;
  EGLConfig eglCfg;
  EGLSurface eglSurf;
  EGLContext eglCtx;
#ifdef USING_EGL_EXT
  OffEglExt *eglExt;
#endif 
#else
  GLFWwindow    *window;
#endif

  GLchar *f_shader_source;
} GLTransitionContext;

#define OFFSET(x) offsetof(GLTransitionContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption gltransition_options[] = {
  { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
  { "offset", "delay before startingtransition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
  { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
  {NULL}
};

FRAMESYNC_DEFINE_CLASS(gltransition, GLTransitionContext, fs);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type)
{
  GLuint shader = glCreateShader(type);
  if (!shader || !glIsShader(shader)) {
    return 0;
  }

  glShaderSource(shader, 1, &shader_source, 0);
  glCompileShader(shader);

  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  return (status == GL_TRUE ? shader : 0);
}

static int build_program(AVFilterContext *ctx)
{
  GLuint v_shader, f_shader;
  GLTransitionContext *c = ctx->priv;
  av_log(ctx, AV_LOG_DEBUG, "build_program\n");
  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
     av_log(ctx, AV_LOG_ERROR, "invalid vertex shader\n");
     return -1;
  }
  av_log(ctx, AV_LOG_DEBUG, "build_shader ok\n");

  char *source = NULL;

  if (c->source) {
    FILE *f = fopen(c->source, "rb");

    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "invalid transition source file \"%s\"\n", c->source);
      return -1;
    }

    fseek(f, 0, SEEK_END);
    unsigned long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    source = malloc(fsize + 1);
    fread(source, fsize, 1, f);
    fclose(f);

    source[fsize] = 0;
  }

  const char *transition_source = source ? source : f_default_transition_source;
  av_log(ctx, AV_LOG_DEBUG, "\n%s\n", transition_source);

  int len = strlen(f_shader_template) + strlen(transition_source);
  c->f_shader_source = av_calloc(len, sizeof(*c->f_shader_source));
  if (!c->f_shader_source) {
    return AVERROR(ENOMEM);
  }

  snprintf(c->f_shader_source, len * sizeof(*c->f_shader_source), f_shader_template, transition_source);
  av_log(ctx, AV_LOG_DEBUG, "\n%s\n", c->f_shader_source);

  if (source) {
    free(source);
    source = NULL;
  }

  if (!(f_shader = build_shader(ctx, c->f_shader_source, GL_FRAGMENT_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "invalid fragment shader\n");
    return -1;
  }

  c->program = glCreateProgram();
  glAttachShader(c->program, v_shader);
  glAttachShader(c->program, f_shader);
  glLinkProgram(c->program);

  GLint status;
  glGetProgramiv(c->program, GL_LINK_STATUS, &status);
  
  av_log(ctx, AV_LOG_DEBUG, "build_program ok\n");
  
  return status == GL_TRUE ? 0 : -1;
}

static void setup_vbo(GLTransitionContext *c)
{
  glGenBuffers(1, &c->posBuf);
  glBindBuffer(GL_ARRAY_BUFFER, c->posBuf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

  GLint loc = glGetAttribLocation(c->program, "position");
  glEnableVertexAttribArray(loc);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

static void setup_tex(AVFilterLink *fromLink)
{
  AVFilterContext     *ctx = fromLink->dst;
  GLTransitionContext *c = ctx->priv;

  { // from
    glGenTextures(1, &c->from);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->from);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

    glUniform1i(glGetUniformLocation(c->program, "from"), 0);
  }

  { // to
    glGenTextures(1, &c->to);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, c->to);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);

    glUniform1i(glGetUniformLocation(c->program, "to"), 1);
  }
}

static void setup_uniforms(AVFilterLink *fromLink)
{
  AVFilterContext     *ctx = fromLink->dst;
  GLTransitionContext *c = ctx->priv;

  c->progress = glGetUniformLocation(c->program, "progress");
  glUniform1f(c->progress, 0.0f);

  // TODO: this should be output ratio
  c->ratio = glGetUniformLocation(c->program, "ratio");
  glUniform1f(c->ratio, fromLink->w / (float)fromLink->h);

  c->_fromR = glGetUniformLocation(c->program, "_fromR");
  glUniform1f(c->_fromR, fromLink->w / (float)fromLink->h);

  // TODO: initialize this in config_props for "to" input
  c->_toR = glGetUniformLocation(c->program, "_toR");
  glUniform1f(c->_toR, fromLink->w / (float)fromLink->h);
}

#ifdef GL_TRANSITION_USING_EGL
// Get the EGL error back as a string. Useful for debugging.
static const char *eglGetErrorStr()
{
    switch (eglGetError())
    {
    case EGL_SUCCESS:
        return "The last function succeeded without error.";
    case EGL_NOT_INITIALIZED:
        return "EGL is not initialized, or could not be initialized, for the "
               "specified EGL display connection.";
    case EGL_BAD_ACCESS:
        return "EGL cannot access a requested resource (for example a context "
               "is bound in another thread).";
    case EGL_BAD_ALLOC:
        return "EGL failed to allocate resources for the requested operation.";
    case EGL_BAD_ATTRIBUTE:
        return "An unrecognized attribute or attribute value was passed in the "
               "attribute list.";
    case EGL_BAD_CONTEXT:
        return "An EGLContext argument does not name a valid EGL rendering "
               "context.";
    case EGL_BAD_CONFIG:
        return "An EGLConfig argument does not name a valid EGL frame buffer "
               "configuration.";
    case EGL_BAD_CURRENT_SURFACE:
        return "The current surface of the calling thread is a window, pixel "
               "buffer or pixmap that is no longer valid.";
    case EGL_BAD_DISPLAY:
        return "An EGLDisplay argument does not name a valid EGL display "
               "connection.";
    case EGL_BAD_SURFACE:
        return "An EGLSurface argument does not name a valid surface (window, "
               "pixel buffer or pixmap) configured for GL rendering.";
    case EGL_BAD_MATCH:
        return "Arguments are inconsistent (for example, a valid context "
               "requires buffers not supplied by a valid surface).";
    case EGL_BAD_PARAMETER:
        return "One or more argument values are invalid.";
    case EGL_BAD_NATIVE_PIXMAP:
        return "A NativePixmapType argument does not refer to a valid native "
               "pixmap.";
    case EGL_BAD_NATIVE_WINDOW:
        return "A NativeWindowType argument does not refer to a valid native "
               "window.";
    case EGL_CONTEXT_LOST:
        return "A power management event has occurred. The application must "
               "destroy all contexts and reinitialise OpenGL ES state and "
               "objects to continue rendering.";
    default:
        break;
    }
    return "Unknown error!";
}

#ifdef USING_EGL_EXT
static int find_display_configuration (AVFilterContext *ctx,OffEglExt* eglExt) 
{
	av_log(ctx, AV_LOG_DEBUG,"find display configuration\n");

	drmModeRes *resources = drmModeGetResources (eglExt->g_device_fd);
	/* It will crash if GPU driver doesn't support DRM/DRI. */
	if(resources==NULL){
		av_log(ctx, AV_LOG_ERROR,"drmModeGetResources failed\n");
		return -1;
	}
	
	// find a connector
	drmModeConnector *connector = NULL;
	for (int i=0; i < resources->count_connectors; i++) {
		drmModeConnector *temp_connector = drmModeGetConnector (eglExt->g_device_fd, resources->connectors[i]);
		// pick the first connected connector
		if (temp_connector->connection == DRM_MODE_CONNECTED) {
			connector = temp_connector;
			break;
		}
		drmModeFreeConnector (temp_connector);
	}
	if (!connector) {
		av_log(ctx, AV_LOG_ERROR,"no connector found\n");
		return -1;
	}
	
	// save the connector_id
	eglExt->connector_id = connector->connector_id;
	// save the first mode
	eglExt->mode_info = connector->modes[0];
	av_log(ctx, AV_LOG_DEBUG,"resolution: %ix%i\n", eglExt->mode_info.hdisplay, eglExt->mode_info.vdisplay);
	
	// find an encoder
	drmModeEncoder *encoder = NULL;
	if (connector->encoder_id) {
		encoder = drmModeGetEncoder (eglExt->g_device_fd, connector->encoder_id);
	}
	
	if (!encoder){
		av_log(ctx, AV_LOG_ERROR," drm no encoder found\n");
		return -1;
	}
	// find a CRTC
	if (encoder->crtc_id) {
		eglExt->crtc = drmModeGetCrtc (eglExt->g_device_fd, encoder->crtc_id);
	}
	
	// clean up
	drmModeFreeEncoder (encoder);
	drmModeFreeConnector (connector);
	drmModeFreeResources (resources);
	
	return 0;
}

static void off_ext_egl_clean_up (AVFilterContext *ctx) {
	GLTransitionContext *c = ctx->priv;
	OffEglExt * eglExt = c->eglExt;
	if(eglExt){ 
		// set the previous crtc
		drmModeSetCrtc (eglExt->g_device_fd, eglExt->crtc->crtc_id, eglExt->crtc->buffer_id, eglExt->crtc->x, eglExt->crtc->y, &eglExt->connector_id, 1, &eglExt->crtc->mode);
		drmModeFreeCrtc (eglExt->crtc);
		
		if (eglExt->previous_bo) {
			drmModeRmFB (eglExt->g_device_fd, eglExt->previous_fb);
			gbm_surface_release_buffer (eglExt->gbm_surface, eglExt->previous_bo);
		}

		if(c->eglSurf)
			eglDestroySurface (c->eglDpy, c->eglSurf);
		if (eglExt->gbm_surface)
			gbm_surface_destroy (eglExt->gbm_surface);
		if(c->eglCtx!=EGL_NO_CONTEXT)
			eglDestroyContext (c->eglDpy, c->eglCtx);
		if(c->eglDpy!=EGL_NO_DISPLAY)
			eglTerminate (c->eglDpy);
		if(eglExt->gbm_device)
			gbm_device_destroy (eglExt->gbm_device);
		if(eglExt->g_device_fd)
			close(eglExt->g_device_fd);
		if(eglExt->configs){
			free(eglExt->configs);
			eglExt->configs=NULL;
		}
		av_freep(&eglExt);
		eglExt=NULL;
	}
}

static int match_config_to_visual(EGLDisplay display, EGLint visualId, EGLConfig *configs, int count)
{
    EGLint id=0;
    for (int i = 0; i < count; ++i)
    {
        if (!eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &id))
            continue;
        if (id == visualId)
            return i;
    }
    return -1;
}

static int off_ext_egl_setup(AVFilterLink *inLink){
	AVFilterContext *ctx = inLink->dst;
  	GLTransitionContext *c = ctx->priv;
	OffEglExt * eglExt = c->eglExt;
	int major, minor;
	if(!eglExt){
		eglExt = av_mallocz(sizeof(OffEglExt));
	}
	if(!eglExt){
		av_log(ctx, AV_LOG_ERROR,"the eglExt ptr is null\n");
		return -1;
	}
	/* Off-Screen Rendering */
	av_log(ctx, AV_LOG_INFO,"************/dev/dri/card0************");
	#ifdef _GNU_SOURCE
	av_log(ctx, AV_LOG_DEBUG,"open with gnu source O_CLOEXEC\n");
	eglExt->g_device_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	#else
	eglExt->g_device_fd = open("/dev/dri/card0", O_RDWR | O_NOCTTY | O_NONBLOCK);
	#endif 
	if(eglExt->g_device_fd<=0){
		av_log(ctx, AV_LOG_DEBUG,"open(/dev/dri/card0) failed\n");
		return -1;
	}
	if (find_display_configuration (ctx,eglExt)<0){
		return -1;
	}
	
	/* Create EGL Context using GBM */
	
	//Create a gbm device for allocating buffers
	eglExt->gbm_device = gbm_create_device (eglExt->g_device_fd);
	if(eglExt->gbm_device==NULL){
		av_log(ctx, AV_LOG_DEBUG,"gbm_create_device failed\n");
		return -1;
	}
	c->eglDpy = eglGetDisplay (eglExt->gbm_device);
	if(c->eglDpy==EGL_NO_DISPLAY){
		av_log(ctx, AV_LOG_DEBUG,"eglGetDisplay return no display failed\n");
		return -1;
	}
	eglInitialize (c->eglDpy, &major, &minor);
	eglBindAPI (EGL_OPENGL_API);
	
    av_log(ctx, AV_LOG_DEBUG,"*************EGL %d.%d when eglGetDisplay(GBM)*************\n", major, minor);
	av_log(ctx, AV_LOG_DEBUG," EGL_CLIENT_APIS: %s\n", eglQueryString(c->eglDpy, EGL_CLIENT_APIS));
	av_log(ctx, AV_LOG_DEBUG," EGL_VENDOR: %s\n", eglQueryString(c->eglDpy,  EGL_VENDOR));
	av_log(ctx, AV_LOG_DEBUG," EGL_VERSION: %s\n", eglQueryString(c->eglDpy,  EGL_VERSION));
	av_log(ctx, AV_LOG_DEBUG," EGL_EXTENSIONS: %s\n", eglQueryString(c->eglDpy,  EGL_EXTENSIONS));
	
	
	/*EGLint attributes[] = {
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_NONE };*/
	EGLint config_attributes[] = {
	    EGL_BLUE_SIZE, 8,
	    EGL_GREEN_SIZE, 8,
	    EGL_RED_SIZE, 8,
	    EGL_DEPTH_SIZE, 8,
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_NONE};
	EGLint numConfigs=0;
	EGLint count=0;
    if (EGL_FALSE==eglGetConfigs(c->eglDpy, NULL, 0, &count)){
		av_log(ctx, AV_LOG_DEBUG,"eglGetConfigs failed,err:%s \n",eglGetErrorStr());
		return -1;
	}     
    eglExt->configs = malloc(count * sizeof(EGLConfig));
	if(EGL_FALSE==eglChooseConfig(c->eglDpy, config_attributes, eglExt->configs, count, &numConfigs)){
		av_log(ctx, AV_LOG_DEBUG,"eglChooseConfig failed \n");
		return -1;
	}
	// I am not exactly sure why the EGL config must match the GBM format.
    // But it works!
	int configs_index = match_config_to_visual(c->eglDpy,GBM_FORMAT_XRGB8888, eglExt->configs, numConfigs);
	if(configs_index<0){
		av_log(ctx, AV_LOG_DEBUG,"match to config format failed.");
		return -1;
	}
	c->eglCfg = eglExt->configs[configs_index]; 
	
	//create context	
	c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, NULL);
	if(c->eglCtx==EGL_NO_CONTEXT){
		av_log(ctx, AV_LOG_DEBUG,"eglCreateContext failed \n");
		return -1;
	}
	
	// create the GBM and EGL surface
	eglExt->gbm_surface = gbm_surface_create (eglExt->gbm_device, inLink->w, inLink->h, 
									GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
	if(eglExt->gbm_surface==NULL){
		av_log(ctx, AV_LOG_DEBUG,"gbm_surface_create failed\n");
		return -1;
	}
	
	c->eglSurf = eglCreateWindowSurface (c->eglDpy, c->eglCfg, eglExt->gbm_surface, NULL);
	if(c->eglSurf==EGL_NO_SURFACE){
		av_log(ctx, AV_LOG_DEBUG,"eglCreateWindowSurface failed \n");
		return -1;
	} 
	
	eglMakeCurrent (c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
	av_log(ctx, AV_LOG_DEBUG,"off ext setup ok\n");
	return 0;
}

#endif


static int normal_egl_setup(AVFilterLink *inLink){
  AVFilterContext *ctx = inLink->dst;
  GLTransitionContext *c = ctx->priv;
  //init EGL
  // 1. Initialize EGL
  c->eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major, minor;
  eglInitialize(c->eglDpy, &major, &minor);
  av_log(ctx, AV_LOG_DEBUG, "%d%d", major, minor);
  // 2. Select an appropriate configuration
  EGLint numConfigs;
  EGLint pbufferAttribs[] = {
      EGL_WIDTH,
      inLink->w,
      EGL_HEIGHT,
      inLink->h,
      EGL_NONE,
  };
  eglChooseConfig(c->eglDpy, configAttribs, &c->eglCfg, 1, &numConfigs);
  // 3. Create a surface
  c->eglSurf = eglCreatePbufferSurface(c->eglDpy, c->eglCfg,
                                       pbufferAttribs);
  // 4. Bind the API
  eglBindAPI(EGL_OPENGL_API);
  // 5. Create a context and make it current
  c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, NULL);
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
  
  return 0;
}

#endif 
static int setup_gl(AVFilterLink *inLink)
{
  AVFilterContext *ctx = inLink->dst;
  GLTransitionContext *c = ctx->priv;


#ifdef GL_TRANSITION_USING_EGL
  //init EGL
  #ifndef USING_EGL_EXT
  	normal_egl_setup(inLink);
  #else
  	if(off_ext_egl_setup(inLink)<0){
		av_log(ctx, AV_LOG_ERROR, "off_ext_egl_setup ERROR");
		return -1;
  	}
  #endif
#else
  //glfw

  glfwWindowHint(GLFW_VISIBLE, 0);
  c->window = glfwCreateWindow(inLink->w, inLink->h, "", NULL, NULL);
  if (!c->window) {
    av_log(ctx, AV_LOG_ERROR, "setup_gl ERROR");
    return -1;
  }
  glfwMakeContextCurrent(c->window);

#endif


#ifndef __APPLE__
#ifndef USING_EGL_EXT
  av_log(ctx, AV_LOG_DEBUG,"glewInit");
  glewExperimental = GL_TRUE;
  glewInit();
#endif
#endif

  av_log(ctx, AV_LOG_DEBUG,"GL Viewport set:%dx%d",inLink->w, inLink->h);
  glViewport(0, 0, inLink->w, inLink->h);
  // Get GL Viewport size and test if it is correct.
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  
  // viewport[2] and viewport[3] are viewport width and height respectively
  av_log(ctx, AV_LOG_DEBUG,"GL Viewport size: %dx%d", viewport[2], viewport[3]);

  int ret;
  if((ret = build_program(ctx)) < 0) {
    return ret;
  }

  glUseProgram(c->program);
  setup_vbo(c);
  setup_uniforms(inLink);
  setup_tex(inLink);

  return 0;
}

static AVFrame *apply_transition(FFFrameSync *fs,
                                 AVFilterContext *ctx,
                                 AVFrame *fromFrame,
                                 const AVFrame *toFrame)
{
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  AVFilterLink *outLink = ctx->outputs[0];
  AVFrame *outFrame;

  outFrame = ff_get_video_buffer(outLink, outLink->w, outLink->h);
  if (!outFrame) {
    return NULL;
  }

  av_frame_copy_props(outFrame, fromFrame);

#ifdef GL_TRANSITION_USING_EGL
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#else
  glfwMakeContextCurrent(c->window);
#endif

  glUseProgram(c->program);

  const float ts = ((fs->pts - c->first_pts) / (float)fs->time_base.den) - c->offset;
  const float progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
  //progress的计算值:ts<0,progress=0;ts>1,progress=1;0<ts<1,progress=ts;
  // av_log(ctx, AV_LOG_ERROR, "transition '%s' %llu %f %f\n", c->source, fs->pts - c->first_pts, ts, progress);
  glUniform1f(c->progress, progress);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->from);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);

  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, c->to);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);

  glDrawArrays(GL_TRIANGLES, 0, 6);
  glReadPixels(0, 0, outLink->w, outLink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)outFrame->data[0]);

  av_frame_free(&fromFrame);

  return outFrame;
}

static int blend_frame(FFFrameSync *fs)
{
  AVFilterContext *ctx = fs->parent;
  GLTransitionContext *c = ctx->priv;

  AVFrame *fromFrame, *toFrame, *outFrame;
  int ret;

  ret = ff_framesync_dualinput_get(fs, &fromFrame, &toFrame);
  if (ret < 0) {
    return ret;
  }

  if (c->first_pts == AV_NOPTS_VALUE && fromFrame && fromFrame->pts != AV_NOPTS_VALUE) {
    c->first_pts = fromFrame->pts;
  }

  if (!toFrame) {
    return ff_filter_frame(ctx->outputs[0], fromFrame);
  }

  outFrame = apply_transition(fs, ctx, fromFrame, toFrame);
  if (!outFrame) {
    return AVERROR(ENOMEM);
  }

  return ff_filter_frame(ctx->outputs[0], outFrame);
}

static av_cold int init(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  c->fs.on_event = blend_frame;
  c->first_pts = AV_NOPTS_VALUE;


#ifndef GL_TRANSITION_USING_EGL
  if (!glfwInit())
  {
    return -1;
  }
#endif

  return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
  GLTransitionContext *c = ctx->priv;
  ff_framesync_uninit(&c->fs);

#ifdef GL_TRANSITION_USING_EGL
  if (c->eglDpy) {
    glDeleteTextures(1, &c->from);
    glDeleteTextures(1, &c->to);
    glDeleteBuffers(1, &c->posBuf);
    glDeleteProgram(c->program);
	#ifndef USING_EGL_EXT
    	eglTerminate(c->eglDpy);
	#else
		off_ext_egl_clean_up(ctx);
	#endif
  }
#else
  if (c->window) {
    glDeleteTextures(1, &c->from);
    glDeleteTextures(1, &c->to);
    glDeleteBuffers(1, &c->posBuf);
    glDeleteProgram(c->program);
    glfwDestroyWindow(c->window);
  }
#endif

  if (c->f_shader_source) {
    av_freep(&c->f_shader_source);
  }
}

static int query_formats(AVFilterContext *ctx)
{
  static const enum AVPixelFormat formats[] = {
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_NONE
  };

  return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static int activate(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  return ff_framesync_activate(&c->fs);
}

static int config_output(AVFilterLink *outLink)
{
  AVFilterContext *ctx = outLink->src;
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  int ret;

  if (fromLink->format != toLink->format) {
    av_log(ctx, AV_LOG_ERROR, "inputs must be of same pixel format\n");
    return AVERROR(EINVAL);
  }

  if (fromLink->w != toLink->w || fromLink->h != toLink->h) {
    av_log(ctx, AV_LOG_ERROR, "First input link %s parameters "
           "(size %dx%d) do not match the corresponding "
           "second input link %s parameters (size %dx%d)\n",
           ctx->input_pads[FROM].name, fromLink->w, fromLink->h,
           ctx->input_pads[TO].name, toLink->w, toLink->h);
    return AVERROR(EINVAL);
  }

  outLink->w = fromLink->w;
  outLink->h = fromLink->h;
  // outLink->time_base = fromLink->time_base;
  outLink->frame_rate = fromLink->frame_rate;

  if ((ret = ff_framesync_init_dualinput(&c->fs, ctx)) < 0) {
    return ret;
  }

  return ff_framesync_configure(&c->fs);
}

static const AVFilterPad gltransition_inputs[] = {
  {
    .name = "from",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = setup_gl,
  },
  {
    .name = "to",
    .type = AVMEDIA_TYPE_VIDEO,
  },
  {NULL}
};

static const AVFilterPad gltransition_outputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_output,
  },
  {NULL}
};

AVFilter ff_vf_gltransition = {
  .name          = "gltransition",
  .description   = NULL_IF_CONFIG_SMALL("OpenGL blend transitions"),
  .priv_size     = sizeof(GLTransitionContext),
  .preinit       = gltransition_framesync_preinit,
  .init          = init,
  .uninit        = uninit,
  .query_formats = query_formats,
  .activate      = activate,
  .inputs        = gltransition_inputs,
  .outputs       = gltransition_outputs,
  .priv_class    = &gltransition_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC
};
