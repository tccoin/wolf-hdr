//! Thin Rust ownership wrapper around the OpenGL 3 / EGL dma-buf bridge.
//!
//! The actual import is C because the public GStreamer GL allocator API is C
//! only. Keep it behind the opt-in `gl-hdr` feature so ordinary Wolf builds do
//! not acquire an extra GL context or alter their SDR path.

use gst::glib;
use gst::glib::translate::{IntoGlib, ToGlibPtr, from_glib_full};
use gst::{Buffer, Element, QueryRef};
use std::fmt;
use std::ptr::NonNull;

#[repr(C)]
struct WolfHdrGlOpaque {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn wolf_hdr_gl_new() -> *mut WolfHdrGlOpaque;
    fn wolf_hdr_gl_free(bridge: *mut WolfHdrGlOpaque);
    fn wolf_hdr_gl_handle_context_query(
        bridge: *mut WolfHdrGlOpaque,
        element: *mut gst::ffi::GstElement,
        query: *mut gst::ffi::GstQuery,
    ) -> glib::ffi::gboolean;
    fn wolf_hdr_gl_import_ab30_dmabuf(
        bridge: *mut WolfHdrGlOpaque,
        input: *mut gst::ffi::GstBuffer,
        width: u32,
        height: u32,
        modifier: u64,
        native_pq: glib::ffi::gboolean,
    ) -> *mut gst::ffi::GstBuffer;
}

pub struct HdrGlBridge {
    raw: NonNull<WolfHdrGlOpaque>,
}

// GStreamer serializes BaseSrc::create() for an element. The C bridge itself
// routes all GL work through GstGLContext's worker thread.
unsafe impl Send for HdrGlBridge {}

impl fmt::Debug for HdrGlBridge {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("HdrGlBridge").finish_non_exhaustive()
    }
}

impl HdrGlBridge {
    pub fn new() -> Option<Self> {
        NonNull::new(unsafe { wolf_hdr_gl_new() }).map(|raw| Self { raw })
    }

    pub fn handle_context_query(&self, element: &Element, query: &mut QueryRef) -> bool {
        unsafe {
            wolf_hdr_gl_handle_context_query(
                self.raw.as_ptr(),
                element.to_glib_none().0,
                query.as_mut_ptr(),
            ) != glib::ffi::GFALSE
        }
    }

    pub fn import_ab30(
        &self,
        input: Buffer,
        width: u32,
        height: u32,
        modifier: u64,
        native_pq: bool,
    ) -> Option<Buffer> {
        let ptr = unsafe {
            wolf_hdr_gl_import_ab30_dmabuf(
                self.raw.as_ptr(),
                input.to_glib_none().0,
                width,
                height,
                modifier,
                native_pq.into_glib(),
            )
        };
        NonNull::new(ptr).map(|ptr| unsafe { from_glib_full(ptr.as_ptr()) })
    }
}

impl Drop for HdrGlBridge {
    fn drop(&mut self) {
        unsafe { wolf_hdr_gl_free(self.raw.as_ptr()) }
    }
}
