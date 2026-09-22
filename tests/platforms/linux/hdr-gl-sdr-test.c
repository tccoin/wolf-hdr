/* GPU regression test: real AB24 GBM dma-buf -> production HDR GL bridge.
 * Build inside the gst-wayland-display development image, with GPU access:
 * cc tests/platforms/linux/hdr-gl-sdr-test.c -o /tmp/hdr-gl-sdr-test \
 *   $(pkg-config --cflags --libs gbm gstreamer-gl-1.0 \
 *     gstreamer-allocators-1.0 egl) -lm
 * WOLF_SDR_REFERENCE_WHITE=100 /tmp/hdr-gl-sdr-test
 */
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <unistd.h>
#include "../../../third_party/gst-wayland-display/gst-plugin-wayland-display/src/waylandsrc/hdr_gl.c"

#define WIDTH 48
#define HEIGHT 16

typedef struct {
  ImportJob import;
  GLuint output_texture;
  float pixels[WIDTH * 4];
  gboolean ok;
} Test;

static const float patches[6][3] = {
  {1, 1, 1}, {128.0f/255, 128.0f/255, 128.0f/255},
  {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}
};

static void
paint_input (GstGLContext *context, gpointer data)
{
  Test *test = data;
  const GstGLFuncs *gl = context->gl_vtable;
  import_ab30_on_gl_thread (context, &test->import);
  if (!test->import.ok)
    return;
  gl->BindFramebuffer (GL_FRAMEBUFFER, test->import.bridge->fbo);
  gl->FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_TEXTURE_2D, test->import.texture, 0);
  if (gl->CheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return;
  gl->Enable (GL_SCISSOR_TEST);
  for (int i = 0; i < 6; ++i) {
    gl->Scissor (i * 8, 0, 8, HEIGHT);
    gl->ClearColor (patches[i][0], patches[i][1], patches[i][2], 1);
    gl->Clear (GL_COLOR_BUFFER_BIT);
  }
  gl->Disable (GL_SCISSOR_TEST);
  gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
  gl->Finish ();
  test->ok = gl->GetError () == GL_NO_ERROR;
}

static void
read_output (GstGLContext *context, gpointer data)
{
  Test *test = data;
  const GstGLFuncs *gl = context->gl_vtable;
  gl->BindFramebuffer (GL_FRAMEBUFFER, test->import.bridge->fbo);
  gl->FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_TEXTURE_2D, test->output_texture, 0);
  gl->ReadPixels (0, HEIGHT/2, WIDTH, 1, GL_RGBA, GL_FLOAT, test->pixels);
  gl->BindFramebuffer (GL_FRAMEBUFFER, 0);
  test->ok = gl->GetError () == GL_NO_ERROR;
}

static double
pq_nits (double value)
{
  double p = pow (value, 1.0 / 78.84375);
  return 10000 * pow (fmax (p - 0.8359375, 0) /
      (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  gboolean native_pq = argc > 1 && g_str_equal (argv[1], "--native-pq");
  int fd = open ("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  g_assert_cmpint (fd, >=, 0);
  struct gbm_device *device = gbm_create_device (fd);
  g_assert_nonnull (device);
  struct gbm_bo *bo = gbm_bo_create (device, WIDTH, HEIGHT,
      native_pq ? GBM_FORMAT_ABGR2101010 : GBM_FORMAT_ABGR8888,
      GBM_BO_USE_RENDERING);
  g_assert_nonnull (bo);
  WolfHdrGl *bridge = wolf_hdr_gl_new ();
  g_assert_nonnull (bridge);
  int dma_fd = gbm_bo_get_fd (bo);
  g_assert_cmpint (dma_fd, >=, 0);
  guint stride = gbm_bo_get_stride (bo);
  guint64 modifier = gbm_bo_get_modifier (bo);
  Test test = { .import = {
    .bridge = bridge, .fd = dma_fd, .width = WIDTH, .height = HEIGHT,
    .stride = stride, .offset = gbm_bo_get_offset (bo, 0),
    .modifier = modifier, .native_pq = TRUE,
    .fourcc = native_pq ? DRM_FORMAT_ABGR2101010 : DRM_FORMAT_ABGR8888,
    .image = EGL_NO_IMAGE_KHR
  }};
  gst_gl_context_thread_add (bridge->context, paint_input, &test);
  g_assert_true (test.ok);
  GstAllocator *allocator = gst_dmabuf_allocator_new ();
  GstBuffer *input = gst_buffer_new ();
  gst_buffer_append_memory (input,
      gst_dmabuf_allocator_alloc (allocator, dma_fd, stride * HEIGHT));
  gsize offsets[GST_VIDEO_MAX_PLANES] = {test.import.offset};
  gint strides[GST_VIDEO_MAX_PLANES] = {stride};
  gst_buffer_add_video_meta_full (input, GST_VIDEO_FRAME_FLAG_NONE,
      native_pq ? GST_VIDEO_FORMAT_RGB10A2_LE : GST_VIDEO_FORMAT_RGBA,
      WIDTH, HEIGHT, 1, offsets, strides);
  /* Even a stale native-PQ hint must not bypass conversion for AB24 SDR. */
  GstBuffer *output = wolf_hdr_gl_import_ab30_dmabuf (bridge, input,
      WIDTH, HEIGHT, modifier, TRUE);
  g_assert_nonnull (output);
  test.output_texture = gst_gl_memory_get_texture_id (
      (GstGLMemory *) gst_buffer_peek_memory (output, 0));
  gst_gl_context_thread_add (bridge->context, read_output, &test);
  g_assert_true (test.ok);

  /* Independent luminance references: neutral sRGB and BT.709 primaries
   * expressed in BT.2020 linear RGB; allow 10-bit PQ quantization. */
  const double expected[6][3] = {
    {1, 1, 1}, {0.2158605, 0.2158605, 0.2158605},
    {0.627404, 0.069097, 0.016391},
    {0.329283, 0.919541, 0.088013},
    {0.043313, 0.011362, 0.895595}, {0, 0, 0}
  };
  const char *names[] = {"white", "gray128", "red", "green", "blue", "black"};
  for (int i = 0; i < 6; ++i) {
    g_print ("%s PQ / decoded BT.2020 nits:", names[i]);
    for (int c = 0; c < 3; ++c) {
      double pq = test.pixels[(i * 8 + 4) * 4 + c];
      if (native_pq) {
        g_print (" %.6f (passthrough)", pq);
        g_assert_cmpfloat (fabs (pq - patches[i][c]), <, 0.001);
        continue;
      }
      double nits = pq_nits (pq);
      double target = expected[i][c] * bridge->sdr_reference_white_nits;
      g_print (" %.6f/%.3f", pq, nits);
      g_assert_cmpfloat (fabs (nits - target), <, fmax (0.02, target * 0.008));
    }
    g_print (" PASS\n");
  }
  gst_buffer_unref (output);
  gst_buffer_unref (input);
  gst_object_unref (allocator);
  TextureCleanup cleanup = {bridge, bridge->context,
      GST_GL_DISPLAY_EGL (gst_gl_context_get_display (bridge->context))->display,
      test.import.image, test.import.texture, test.import.source_texture};
  gst_gl_context_thread_add (bridge->context, destroy_texture_on_gl_thread, &cleanup);
  wolf_hdr_gl_free (bridge);
  gbm_bo_destroy (bo);
  gbm_device_destroy (device);
  close (fd);
  return 0;
}
