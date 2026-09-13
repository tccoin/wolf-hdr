fn main() {
    gst_plugin_version_helper::info();

    if std::env::var_os("CARGO_FEATURE_GL_HDR").is_some() {
        let gl = pkg_config::Config::new()
            .atleast_version("1.24")
            .probe("gstreamer-gl-1.0")
            .expect("gstreamer-gl-1.0 development files are required for gl-hdr");
        let mut build = cc::Build::new();
        build.file("src/waylandsrc/hdr_gl.c");
        for include in gl.include_paths {
            build.include(include);
        }
        build.compile("wolf_hdr_gl");
        println!("cargo:rerun-if-changed=src/waylandsrc/hdr_gl.c");
        println!("cargo:rerun-if-changed=src/waylandsrc/hdr_gl.h");
    }
}
