#include "hdr_gl.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm/drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gstglcontext.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglutils.h>
#include <gst/video/video.h>

struct WolfHdrGl {
  GstGLDisplay *display;
  GstGLContext *context;
  GLuint sdr_to_pq_program;
  GLuint fullscreen_vao;
  GLuint fbo;
  GLint input_uniform;
  GLint reference_white_uniform;
  gfloat sdr_reference_white_nits;
  gboolean transform_ready;
};

typedef struct {
  WolfHdrGl *bridge;
  EGLImageKHR image;
  GLuint texture;
  gboolean ok;
  gint fd;
  guint width;
  guint height;
  guint stride;
  guint offset;
  guint64 modifier;
  gboolean native_pq;
  GLuint source_texture;
} ImportJob;

typedef struct {
  GstGLContext *context;
  EGLDisplay display;
  EGLImageKHR image;
  GLuint texture;
  GLuint source_texture;
} TextureCleanup;

static const gchar sdr_to_pq_vertex_shader[] =
    "#version 330\n"
    "out vec2 v_uv;\n"
    "const vec2 p[3] = vec2[](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));\n"
    "void main() { gl_Position = vec4(p[gl_VertexID], 0.0, 1.0); v_uv = (p[gl_VertexID] + 1.0) * 0.5; }\n";

static const gchar sdr_to_pq_fragment_shader[] =
    "#version 330\n"
    "uniform sampler2D u_input; uniform float u_reference_white_nits;\n"
    "in vec2 v_uv; layout(location=0) out vec4 out_color;\n"
    "vec3 srgb_to_linear(vec3 c) {\n"
    "  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));\n"
    "}\n"
    "float pq_oetf(float x) {\n"
    "  const float m1=0.1593017578125, m2=78.84375, c1=0.8359375, c2=18.8515625, c3=18.6875;\n"
    "  float xm=pow(max(x, 0.0), m1); return pow((c1+c2*xm)/(1.0+c3*xm), m2);\n"
    "}\n"
    "void main() {\n"
    "  vec3 rgb = texture(u_input, v_uv).rgb;\n"
    "  mat3 m = mat3(0.627404,0.069097,0.016391, 0.329283,0.919541,0.088013, 0.043313,0.011362,0.895595);\n"
    "  vec3 linear2020 = m * srgb_to_linear(rgb);\n"
    "  vec3 pq = vec3(pq_oetf(clamp(linear2020.r,0.0,1.0)*u_reference_white_nits/10000.0),\n"
    "                 pq_oetf(clamp(linear2020.g,0.0,1.0)*u_reference_white_nits/10000.0),\n"
    "                 pq_oetf(clamp(linear2020.b,0.0,1.0)*u_reference_white_nits/10000.0));\n"
    "  out_color = vec4(pq, 1.0);\n"
    "}\n";

static GLuint
compile_shader (const GstGLFuncs *gl, GLenum type, const gchar *source)
{
  GLuint shader = gl->CreateShader (type);
  GLint status = GL_FALSE;

  gl->ShaderSource (shader, 1, &source, NULL);
  gl->CompileShader (shader);
  gl->GetShaderiv (shader, GL_COMPILE_STATUS, &status);
  if (status != GL_TRUE) {
    gchar log[2048] = { 0, };
    gl->GetShaderInfoLog (shader, sizeof (log) - 1, NULL, log);
    GST_ERROR ("HDR GL bridge: SDR-to-PQ shader compilation failed: %s", log);
    gl->DeleteShader (shader);
    return 0;
  }
  return shader;
}

static void
prepare_sdr_transform_on_gl_thread (GstGLContext *context, gpointer user_data)
{
  WolfHdrGl *bridge = user_data;
  const GstGLFuncs *gl = context->gl_vtable;
  GLuint vertex = 0, fragment = 0;
  GLint status = GL_FALSE;

  vertex = compile_shader (gl, GL_VERTEX_SHADER, sdr_to_pq_vertex_shader);
  fragment = compile_shader (gl, GL_FRAGMENT_SHADER, sdr_to_pq_fragment_shader);
  if (vertex == 0 || fragment == 0)
    goto done;

  bridge->sdr_to_pq_program = gl->CreateProgram ();
  gl->AttachShader (bridge->sdr_to_pq_program, vertex);
  gl->AttachShader (bridge->sdr_to_pq_program, fragment);
  gl->LinkProgram (bridge->sdr_to_pq_program);
  gl->GetProgramiv (bridge->sdr_to_pq_program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    gchar log[2048] = { 0, };
    gl->GetProgramInfoLog (bridge->sdr_to_pq_program, sizeof (log) - 1, NULL, log);
    GST_ERROR ("HDR GL bridge: SDR-to-PQ shader link failed: %s", log);
    gl->DeleteProgram (bridge->sdr_to_pq_program);
    bridge->sdr_to_pq_program = 0;
    goto done;
  }
  bridge->input_uniform = gl->GetUniformLocation (bridge->sdr_to_pq_program, "u_input");
  bridge->reference_white_uniform = gl->GetUniformLocation (bridge->sdr_to_pq_program,
      "u_reference_white_nits");
  gl->GenVertexArrays (1, &bridge->fullscreen_vao);
  gl->GenFramebuffers (1, &bridge->fbo);
  bridge->transform_ready = bridge->fullscreen_vao != 0 && bridge->fbo != 0;
  if (!bridge->transform_ready)
    GST_ERROR ("HDR GL bridge: failed to allocate SDR-to-PQ render objects");

done:
  if (vertex != 0)
    gl->DeleteShader (vertex);
  if (fragment != 0)
    gl->DeleteShader (fragment);
}

static void
destroy_texture_on_gl_thread (GstGLContext *context, gpointer user_data)
{
  TextureCleanup *cleanup = user_data;
  const GstGLFuncs *gl = context->gl_vtable;

  if (cleanup->texture != 0)
    gl->DeleteTextures (1, &cleanup->texture);
  if (cleanup->source_texture != 0 && cleanup->source_texture != cleanup->texture)
    gl->DeleteTextures (1, &cleanup->source_texture);
  if (cleanup->image != EGL_NO_IMAGE_KHR) {
    PFNEGLDESTROYIMAGEKHRPROC destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress ("eglDestroyImageKHR");
    if (destroy_image != NULL)
      destroy_image (cleanup->display, cleanup->image);
  }
}

static void
free_texture_cleanup (gpointer user_data)
{
  TextureCleanup *cleanup = user_data;

  /* GstGLMemory invokes its user notify after its own GL-thread cleanup. Keep
   * a context reference here so deleting our wrapped texture remains safe. */
  gst_gl_context_thread_add (cleanup->context, destroy_texture_on_gl_thread,
      cleanup);
  gst_object_unref (cleanup->context);
  g_free (cleanup);
}

static void
import_ab30_on_gl_thread (GstGLContext *context, gpointer user_data)
{
  ImportJob *job = user_data;
  GstGLDisplayEGL *display_egl = GST_GL_DISPLAY_EGL (
      gst_gl_context_get_display (context));
  EGLDisplay display = display_egl->display;
  const GstGLFuncs *gl = context->gl_vtable;
  PFNEGLCREATEIMAGEKHRPROC create_image;

  create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress ("eglCreateImageKHR");
  if (create_image == NULL || gl->EGLImageTargetTexture2D == NULL) {
    GST_ERROR ("HDR GL bridge: EGL dma-buf import is unavailable");
    return;
  }

  const EGLint attributes[] = {
    EGL_WIDTH, (EGLint) job->width,
    EGL_HEIGHT, (EGLint) job->height,
    EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR2101010,
    EGL_DMA_BUF_PLANE0_FD_EXT, job->fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint) job->offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) job->stride,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint) (job->modifier & G_GUINT64_CONSTANT (0xffffffff)),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint) (job->modifier >> 32),
    EGL_NONE
  };

  job->image = create_image (display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
      NULL, attributes);
  if (job->image == EGL_NO_IMAGE_KHR) {
    GST_ERROR ("HDR GL bridge: eglCreateImageKHR(AB30) failed (0x%x)", eglGetError ());
    return;
  }

  gl->GenTextures (1, &job->texture);
  job->source_texture = job->texture;
  gl->BindTexture (GL_TEXTURE_2D, job->source_texture);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl->EGLImageTargetTexture2D (GL_TEXTURE_2D, job->image);
  gl->BindTexture (GL_TEXTURE_2D, 0);

  if (!job->native_pq) {
    GLuint output_texture = 0;
    if (!job->bridge->transform_ready) {
      GST_ERROR ("HDR GL bridge: SDR-to-PQ transform is unavailable");
      return;
    }
    gl->GenTextures (1, &output_texture);
    gl->BindTexture (GL_TEXTURE_2D, output_texture);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->TexImage2D (GL_TEXTURE_2D, 0, GL_RGB10_A2, job->width, job->height, 0,
        GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, NULL);
    gl->BindFramebuffer (GL_FRAMEBUFFER, job->bridge->fbo);
    gl->FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        output_texture, 0);
    if (gl->CheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      GST_ERROR ("HDR GL bridge: SDR-to-PQ framebuffer is incomplete");
      gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
      gl->DeleteTextures (1, &output_texture);
      return;
    }
    gl->Viewport (0, 0, job->width, job->height);
    gl->UseProgram (job->bridge->sdr_to_pq_program);
    gl->ActiveTexture (GL_TEXTURE0);
    gl->BindTexture (GL_TEXTURE_2D, job->source_texture);
    gl->Uniform1i (job->bridge->input_uniform, 0);
    gl->Uniform1f (job->bridge->reference_white_uniform,
        job->bridge->sdr_reference_white_nits);
    gl->BindVertexArray (job->bridge->fullscreen_vao);
    gl->DrawArrays (GL_TRIANGLES, 0, 3);
    gl->BindVertexArray (0);
    gl->BindTexture (GL_TEXTURE_2D, 0);
    gl->UseProgram (0);
    gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
    job->texture = output_texture;
  }
  job->ok = TRUE;
}

WolfHdrGl *
wolf_hdr_gl_new (void)
{
  GError *error = NULL;
  WolfHdrGl *bridge = g_new0 (WolfHdrGl, 1);

  bridge->display = GST_GL_DISPLAY (gst_gl_display_egl_new_surfaceless ());
  if (bridge->display == NULL)
    goto fail;

  /* nvh265enc's GL interop supports desktop OpenGL 3, not GLES. */
  gst_gl_display_filter_gl_api (bridge->display, GST_GL_API_OPENGL3);
  bridge->context = gst_gl_context_new (bridge->display);
  if (bridge->context == NULL ||
      !gst_gl_context_create (bridge->context, NULL, &error)) {
    GST_ERROR ("HDR GL bridge: failed to create OpenGL 3 context: %s",
        error ? error->message : "unknown error");
    g_clear_error (&error);
    goto fail;
  }

  bridge->sdr_reference_white_nits = 203.0f;
  if (g_getenv ("WOLF_SDR_REFERENCE_WHITE") != NULL) {
    gchar *end = NULL;
    gdouble value = g_ascii_strtod (g_getenv ("WOLF_SDR_REFERENCE_WHITE"), &end);
    if (end != g_getenv ("WOLF_SDR_REFERENCE_WHITE") && value > 0.0 && value <= 1000.0)
      bridge->sdr_reference_white_nits = (gfloat) value;
  }
  gst_gl_context_thread_add (bridge->context, prepare_sdr_transform_on_gl_thread, bridge);
  if (!bridge->transform_ready)
    goto fail;

  GST_INFO ("HDR GL bridge: created dedicated OpenGL 3 context with SDR-to-PQ transform (%.1f nits white)",
      bridge->sdr_reference_white_nits);
  return bridge;

fail:
  wolf_hdr_gl_free (bridge);
  return NULL;
}

void
wolf_hdr_gl_free (WolfHdrGl *bridge)
{
  if (bridge == NULL)
    return;
  gst_clear_object (&bridge->context);
  gst_clear_object (&bridge->display);
  g_free (bridge);
}

gboolean
wolf_hdr_gl_handle_context_query (WolfHdrGl *bridge, GstElement *element,
    GstQuery *query)
{
  return bridge != NULL && gst_gl_handle_context_query (element, query,
      bridge->display, bridge->context, NULL);
}

GstBuffer *
wolf_hdr_gl_import_ab30_dmabuf (WolfHdrGl *bridge, GstBuffer *input,
    guint width, guint height, guint64 modifier, gboolean native_pq)
{
  GstVideoMeta *meta;
  GstMemory *memory;
  ImportJob job = { 0, };
  GstVideoInfo video_info;
  GstGLVideoAllocationParams *params;
  GstGLMemoryAllocator *allocator;
  GstBuffer *output;
  TextureCleanup *cleanup;
  gpointer wrapped_texture[1];

  if (bridge == NULL || input == NULL || gst_buffer_n_memory (input) != 1)
    return NULL;
  memory = gst_buffer_peek_memory (input, 0);
  if (!gst_is_dmabuf_memory (memory)) {
    GST_ERROR ("HDR GL bridge: compositor did not return dma-buf memory");
    return NULL;
  }
  meta = gst_buffer_get_video_meta (input);
  if (meta == NULL || meta->n_planes != 1) {
    GST_ERROR ("HDR GL bridge: AB30 buffer lacks single-plane video metadata");
    return NULL;
  }

  job.bridge = bridge;
  job.fd = gst_dmabuf_memory_get_fd (memory);
  job.width = width;
  job.height = height;
  job.stride = meta->stride[0];
  job.offset = meta->offset[0];
  job.modifier = modifier;
  job.native_pq = native_pq;
  job.image = EGL_NO_IMAGE_KHR;
  gst_gl_context_thread_add (bridge->context, import_ab30_on_gl_thread, &job);
  if (!job.ok) {
    if (job.image != EGL_NO_IMAGE_KHR) {
      TextureCleanup orphan = { bridge->context,
          GST_GL_DISPLAY_EGL (gst_gl_context_get_display (bridge->context))->display,
          job.image, job.texture, job.source_texture };
      destroy_texture_on_gl_thread (bridge->context, &orphan);
    }
    return NULL;
  }

  gst_video_info_init (&video_info);
  if (!gst_video_info_set_format (&video_info, GST_VIDEO_FORMAT_RGB10A2_LE,
          width, height)) {
    GST_ERROR ("HDR GL bridge: failed to initialize RGB10A2 video info");
    return NULL;
  }

  cleanup = g_new0 (TextureCleanup, 1);
  cleanup->context = GST_GL_CONTEXT (gst_object_ref (bridge->context));
  cleanup->display = GST_GL_DISPLAY_EGL (
      gst_gl_context_get_display (bridge->context))->display;
  cleanup->image = job.image;
  cleanup->texture = job.texture;
  cleanup->source_texture = job.source_texture;

  params = gst_gl_video_allocation_params_new_wrapped_texture (bridge->context,
      NULL, &video_info, 0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGB10_A2,
      job.texture, cleanup, free_texture_cleanup);
  if (params == NULL) {
    free_texture_cleanup (cleanup);
    return NULL;
  }
  allocator = gst_gl_memory_allocator_get_default (bridge->context);
  output = gst_buffer_new ();
  wrapped_texture[0] = GUINT_TO_POINTER (job.texture);
  if (!gst_gl_memory_setup_buffer (allocator, output, params, NULL,
          wrapped_texture, 1)) {
    gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
    gst_buffer_unref (output);
    return NULL;
  }
  gst_gl_allocation_params_free ((GstGLAllocationParams *) params);

  /* Keep the compositor's fd-backed buffer alive until NVENC releases the
   * imported texture. */
  gst_buffer_add_parent_buffer_meta (output, input);
  return output;
}
