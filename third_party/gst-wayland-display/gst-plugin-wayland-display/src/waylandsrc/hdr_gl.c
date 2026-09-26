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
  gint ref_count;
  GstGLDisplay *display;
  GstGLContext *context;
  GMutex cleanup_lock;
  GPtrArray *retired_cleanups;
  GLuint sdr_to_pq_program;
  GLuint pq_copy_program;
  GLuint fullscreen_vao;
  GLuint fbo;
  GLint input_uniform;
  GLint pq_copy_input_uniform;
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
  guint32 fourcc;
  GLuint source_texture;
} ImportJob;

typedef struct {
  WolfHdrGl *bridge;
  GstGLContext *context;
  EGLDisplay display;
  EGLImageKHR image;
  GLuint texture;
  GLuint source_texture;
  /* The downstream encoder may retain/copy wrapped GL memory while recycling
   * buffers. Keep teardown idempotent and defer descriptor deallocation to
   * the owning bridge, after its pipeline has stopped. */
  gint released;
} TextureCleanup;

static WolfHdrGl *
wolf_hdr_gl_ref (WolfHdrGl *bridge)
{
  g_atomic_int_inc (&bridge->ref_count);
  return bridge;
}

static void
wolf_hdr_gl_unref (WolfHdrGl *bridge)
{
  if (bridge == NULL || !g_atomic_int_dec_and_test (&bridge->ref_count))
    return;
  if (bridge->retired_cleanups != NULL)
    g_ptr_array_unref (bridge->retired_cleanups);
  g_mutex_clear (&bridge->cleanup_lock);
  gst_clear_object (&bridge->context);
  gst_clear_object (&bridge->display);
  g_free (bridge);
}

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

/* An imported native-PQ dma-buf is reused by the compositor. Give NVENC an
 * independent texture so its asynchronous read cannot overlap the next frame. */
static const gchar pq_copy_fragment_shader[] =
    "#version 330\n"
    "uniform sampler2D u_input; in vec2 v_uv; layout(location=0) out vec4 out_color;\n"
    "void main() { out_color = texture(u_input, v_uv); }\n";

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
  GLuint vertex = 0, fragment = 0, copy_fragment = 0;
  GLint status = GL_FALSE;

  vertex = compile_shader (gl, GL_VERTEX_SHADER, sdr_to_pq_vertex_shader);
  fragment = compile_shader (gl, GL_FRAGMENT_SHADER, sdr_to_pq_fragment_shader);
  copy_fragment = compile_shader (gl, GL_FRAGMENT_SHADER, pq_copy_fragment_shader);
  if (vertex == 0 || fragment == 0 || copy_fragment == 0)
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
  bridge->pq_copy_program = gl->CreateProgram ();
  gl->AttachShader (bridge->pq_copy_program, vertex);
  gl->AttachShader (bridge->pq_copy_program, copy_fragment);
  gl->LinkProgram (bridge->pq_copy_program);
  gl->GetProgramiv (bridge->pq_copy_program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    gchar log[2048] = { 0, };
    gl->GetProgramInfoLog (bridge->pq_copy_program, sizeof (log) - 1, NULL, log);
    GST_ERROR ("HDR GL bridge: native-PQ copy shader link failed: %s", log);
    gl->DeleteProgram (bridge->pq_copy_program);
    bridge->pq_copy_program = 0;
    goto done;
  }
  bridge->pq_copy_input_uniform = gl->GetUniformLocation (bridge->pq_copy_program,
      "u_input");
  gl->GenVertexArrays (1, &bridge->fullscreen_vao);
  gl->GenFramebuffers (1, &bridge->fbo);
  bridge->transform_ready = bridge->fullscreen_vao != 0 && bridge->fbo != 0 &&
      bridge->pq_copy_program != 0;
  if (!bridge->transform_ready)
    GST_ERROR ("HDR GL bridge: failed to allocate SDR-to-PQ render objects");

done:
  if (vertex != 0)
    gl->DeleteShader (vertex);
  if (fragment != 0)
    gl->DeleteShader (fragment);
  if (copy_fragment != 0)
    gl->DeleteShader (copy_fragment);
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

  if (!g_atomic_int_compare_and_exchange (&cleanup->released, 0, 1)) {
    GST_WARNING ("HDR GL bridge: duplicate wrapped-texture destroy notify");
    return;
  }

  /* GstGLMemory invokes its user notify after its own GL-thread cleanup. Keep
   * a context reference here so deleting our wrapped texture remains safe. */
  gst_gl_context_thread_add (cleanup->context, destroy_texture_on_gl_thread,
      cleanup);
  gst_object_unref (cleanup->context);
  g_mutex_lock (&cleanup->bridge->cleanup_lock);
  g_ptr_array_add (cleanup->bridge->retired_cleanups, cleanup);
  g_mutex_unlock (&cleanup->bridge->cleanup_lock);
  /* Releases the reference acquired when this texture was handed to
   * GStreamer. The last notification owns the final bridge teardown. */
  wolf_hdr_gl_unref (cleanup->bridge);
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
    EGL_LINUX_DRM_FOURCC_EXT, (EGLint) job->fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT, job->fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint) job->offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) job->stride,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint) (job->modifier & G_GUINT64_CONSTANT (0xffffffff)),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint) (job->modifier >> 32),
    EGL_NONE
  };
  /* NVIDIA advertises some block-linear AB30 modifiers through dma-buf, but
   * rejects those modifier attributes when importing into this headless EGL
   * display. Retry the legacy import form on that path; the driver can then
   * select its compatible layout instead of failing the entire HDR stream. */
  const EGLint fallback_attributes[] = {
    EGL_WIDTH, (EGLint) job->width,
    EGL_HEIGHT, (EGLint) job->height,
    EGL_LINUX_DRM_FOURCC_EXT, (EGLint) job->fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT, job->fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint) job->offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) job->stride,
    EGL_NONE
  };

  job->image = create_image (display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
      NULL, attributes);
  if (job->image == EGL_NO_IMAGE_KHR) {
    EGLint modifier_error = eglGetError ();
    GST_WARNING ("HDR GL bridge: modifier-aware AB30 import failed (0x%x); retrying compatibility import",
        modifier_error);
    job->image = create_image (display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
        NULL, fallback_attributes);
    if (job->image == EGL_NO_IMAGE_KHR) {
      GST_ERROR ("HDR GL bridge: compatibility eglCreateImageKHR(AB30) failed (0x%x)", eglGetError ());
      return;
    }
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

  {
    GLuint output_texture = 0;
    if (!job->bridge->transform_ready) {
      GST_ERROR ("HDR GL bridge: HDR transform is unavailable");
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
      GST_ERROR ("HDR GL bridge: HDR framebuffer is incomplete");
      gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
      gl->DeleteTextures (1, &output_texture);
      return;
    }
    gl->Viewport (0, 0, job->width, job->height);
    gl->UseProgram (job->native_pq ? job->bridge->pq_copy_program :
        job->bridge->sdr_to_pq_program);
    gl->ActiveTexture (GL_TEXTURE0);
    gl->BindTexture (GL_TEXTURE_2D, job->source_texture);
    gl->Uniform1i (job->native_pq ? job->bridge->pq_copy_input_uniform :
        job->bridge->input_uniform, 0);
    if (!job->native_pq)
      gl->Uniform1f (job->bridge->reference_white_uniform,
          job->bridge->sdr_reference_white_nits);
    gl->BindVertexArray (job->bridge->fullscreen_vao);
    gl->DrawArrays (GL_TRIANGLES, 0, 3);
    gl->BindVertexArray (0);
    gl->BindTexture (GL_TEXTURE_2D, 0);
    gl->UseProgram (0);
    gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
    /* Complete the sample/copy before the compositor reuses its dma-buf and
     * before CUDA/NVENC reads this frame's private texture. */
    gl->Finish ();
    job->texture = output_texture;
  }
  job->ok = TRUE;
}

WolfHdrGl *
wolf_hdr_gl_new (void)
{
  GError *error = NULL;
  WolfHdrGl *bridge = g_new0 (WolfHdrGl, 1);

  g_atomic_int_set (&bridge->ref_count, 1);
  g_mutex_init (&bridge->cleanup_lock);
  bridge->retired_cleanups = g_ptr_array_new_with_free_func (g_free);

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
  wolf_hdr_gl_unref (bridge);
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
  switch (meta->format) {
    case GST_VIDEO_FORMAT_RGBA:
      job.fourcc = DRM_FORMAT_ABGR8888;
      job.native_pq = FALSE;
      break;
    case GST_VIDEO_FORMAT_RGB10A2_LE:
      job.fourcc = DRM_FORMAT_ABGR2101010;
      job.native_pq = native_pq;
      break;
    default:
      GST_ERROR ("HDR GL bridge: unsupported input format %s",
          gst_video_format_to_string (meta->format));
      return NULL;
  }
  job.image = EGL_NO_IMAGE_KHR;
  gst_gl_context_thread_add (bridge->context, import_ab30_on_gl_thread, &job);
  if (!job.ok) {
    if (job.image != EGL_NO_IMAGE_KHR) {
      TextureCleanup orphan = { bridge, bridge->context,
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
  cleanup->bridge = wolf_hdr_gl_ref (bridge);
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
  /* The default for a desktop GL 3 context is the PBO allocator.  That
   * allocator adds a CPU download path for wrapped textures; this bridge
   * hands NVENC an EGL-imported/output-only texture and must remain entirely
   * on the GL path.  In the SDR-to-PQ case the PBO path corrupts its wrapped
   * texture lifetime as buffers are recycled. */
  allocator = GST_GL_MEMORY_ALLOCATOR (
      gst_allocator_find (GST_GL_MEMORY_ALLOCATOR_NAME));
  if (allocator == NULL) {
    GST_ERROR ("HDR GL bridge: GLMemory allocator is unavailable");
    free_texture_cleanup (cleanup);
    return NULL;
  }
  output = gst_buffer_new ();
  wrapped_texture[0] = GUINT_TO_POINTER (job.texture);
  if (!gst_gl_memory_setup_buffer (allocator, output, params, NULL,
          wrapped_texture, 1)) {
    gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
    gst_buffer_unref (output);
    gst_object_unref (allocator);
    return NULL;
  }
  gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
  gst_object_unref (allocator);

  /* Keep the compositor's fd-backed buffer alive until NVENC releases the
   * imported texture. */
  gst_buffer_add_parent_buffer_meta (output, input);
  return output;
}
