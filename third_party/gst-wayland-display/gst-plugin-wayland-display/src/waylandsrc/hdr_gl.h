#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct WolfHdrGl WolfHdrGl;

/*
 * A small OpenGL 3 bridge owned by waylanddisplaysrc.  The compositor exports
 * an AB30 dma-buf, which this bridge imports as an EGL image and exposes as
 * RGB10A2 GLMemory.  NVENC's GL path then registers that texture directly.
 */
WolfHdrGl *wolf_hdr_gl_new (void);
void wolf_hdr_gl_free (WolfHdrGl *bridge);

gboolean wolf_hdr_gl_handle_context_query (WolfHdrGl *bridge,
    GstElement *element, GstQuery *query);

GstBuffer *wolf_hdr_gl_import_ab30_dmabuf (WolfHdrGl *bridge,
    GstBuffer *input, guint width, guint height, guint64 modifier,
    gboolean native_pq);

G_END_DECLS
