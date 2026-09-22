/* Numeric PQ calibration client; compile with generated xdg-shell and color
 * management protocol bindings. No game/profile state is touched. */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "xdg-shell-client.h"
#include "color-management-client.h"
#include "gamescope-swapchain-client.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct wp_color_manager_v1 *manager;
static struct gamescope_swapchain_factory_v2 *swapchain_factory;
static struct gamescope_swapchain *swapchain;
static int configured, ready;
static unsigned peak, reference;
static int ignore(const void *i, void *target, uint32_t op,
                  const struct wl_message *msg, union wl_argument *a) {
    if (!strcmp(msg->name, "ping")) xdg_wm_base_pong(target, a[0].u);
    if (!strcmp(msg->name, "ready")) ready = 1;
    if (!strcmp(msg->name, "failed")) { fprintf(stderr, "image description failed\n"); abort(); }
    return 0;
}
static void global(void *d, struct wl_registry *r, uint32_t n, const char *s, uint32_t v) {
    if (!strcmp(s, "wl_compositor")) compositor = wl_registry_bind(r, n, &wl_compositor_interface, 4);
    if (!strcmp(s, "wl_shm")) shm = wl_registry_bind(r, n, &wl_shm_interface, 1);
    if (!strcmp(s, "xdg_wm_base")) {
        shell = wl_registry_bind(r, n, &xdg_wm_base_interface, 1);
        wl_proxy_add_dispatcher((void *)shell, ignore, NULL, NULL);
    }
    if (!strcmp(s, "wp_color_manager_v1")) {
        manager = wl_registry_bind(r, n, &wp_color_manager_v1_interface, 1);
        wl_proxy_add_dispatcher((void *)manager, ignore, NULL, NULL);
    }
    if (!strcmp(s, "gamescope_swapchain_factory_v2"))
        swapchain_factory = wl_registry_bind(r, n, &gamescope_swapchain_factory_v2_interface, 1);
}
static void removed(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void configure(void *d, struct xdg_surface *s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial); configured = 1;
}
static const struct xdg_surface_listener surface_listener = {configure};
static int info(const void *i, void *target, uint32_t op,
                const struct wl_message *msg, union wl_argument *a) {
    if (!strcmp(msg->name, "luminances")) reference = a[2].u;
    if (!strcmp(msg->name, "target_luminance")) peak = a[1].u;
    if (!strcmp(msg->name, "done")) wl_proxy_destroy(target);
    return 0;
}
static double pq(double nits) {
    double p = pow(nits / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625*p) / (1 + 18.6875*p), 78.84375);
}
int main(int argc, char **argv) {
    const int depth = getenv("WOLF_PQ_SOURCE_DEPTH") ? atoi(getenv("WOLF_PQ_SOURCE_DEPTH")) : 8;
    assert(depth == 8 || depth == 10);
    struct wl_display *display = wl_display_connect(getenv("GAMESCOPE_WAYLAND_DISPLAY")); assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    assert(wl_display_roundtrip(display) >= 0); assert(compositor && shm && shell && (manager || swapchain_factory));
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    const int width=1280, height=720, size=width*height*4;
    Display *xdisplay = NULL;
    Window xwindow = 0;
    if (!manager) {
        /* Gamescope's private WSI protocol overrides an XWayland window; it
         * is not a color-management protocol for a native xdg-shell client. */
        xdisplay = XOpenDisplay(NULL); assert(xdisplay);
        Window root = DefaultRootWindow(xdisplay);
        xwindow = XCreateSimpleWindow(xdisplay, root, 0, 0, width, height, 0, 0, 0);
        XStoreName(xdisplay, xwindow, "Wolf X11 WSI PQ calibration");
        Atom fullscreen = XInternAtom(xdisplay, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(xdisplay, xwindow, XInternAtom(xdisplay, "_NET_WM_STATE", False),
                        XA_ATOM, 32, PropModeReplace, (unsigned char *)&fullscreen, 1);
        unsigned long pid = getpid();
        XChangeProperty(xdisplay, xwindow, XInternAtom(xdisplay, "_NET_WM_PID", False),
                        XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
        XMapRaised(xdisplay, xwindow); XSync(xdisplay, False);
        Atom actual_type; int actual_format; unsigned long count, remaining;
        unsigned char *server_id_data = NULL;
        unsigned server_id = 0;
        if (XGetWindowProperty(xdisplay, root, XInternAtom(xdisplay, "GAMESCOPE_XWAYLAND_SERVER_ID", False),
                0, 1, False, XA_CARDINAL, &actual_type, &actual_format, &count, &remaining,
                &server_id_data) == Success && actual_format == 32 && count == 1)
            server_id = *(unsigned long *)server_id_data;
        if (server_id_data) XFree(server_id_data);
        swapchain = gamescope_swapchain_factory_v2_create_swapchain(swapchain_factory, surface);
        wl_proxy_add_dispatcher((void *)swapchain, ignore, NULL, NULL);
        /* Same metadata channel as the game's Gamescope Vulkan WSI layer:
         * A2B10G10R10_UNORM or B8G8R8A8_UNORM, HDR10_ST2084,
         * opaque, identity transform. */
        gamescope_swapchain_swapchain_feedback(swapchain, 2, depth == 10 ? 64 : 44, 1000104008, 1, 1, 1, "Wolf PQ regression");
        gamescope_swapchain_set_hdr_metadata(swapchain, 35400, 14600, 8500, 39850,
                                            6550, 2300, 15635, 16450, 550, 0, 550, 300);
        gamescope_swapchain_override_window_content(swapchain, server_id, xwindow);
    }
    if (manager) {
        struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(shell, surface);
        xdg_surface_add_listener(xdg, &surface_listener, NULL);
        struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
        wl_proxy_add_dispatcher((void *)top, ignore, NULL, NULL);
        xdg_toplevel_set_title(top, "Wolf PQ calibration");
        xdg_toplevel_set_fullscreen(top, NULL);
        wl_surface_commit(surface);
        while (!configured) assert(wl_display_roundtrip(display) >= 0);
    }
    int fd=memfd_create("wolf-pq-patches",0); assert(fd>=0); assert(!ftruncate(fd,size));
    uint32_t *pixels=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); assert(pixels!=MAP_FAILED);
    const double levels[]={0,100,203,300,400,550,700,1000};
    for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
        uint32_t code=lround(pq(levels[x/(width/8)])*((1u << depth)-1));
        pixels[y*width+x]=depth == 10 ? (3u<<30 | code<<20 | code<<10 | code)
                                     : (0xff000000 | code<<16 | code<<8 | code);
    }
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,size);
    struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,
        depth == 10 ? WL_SHM_FORMAT_ABGR2101010 : WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(surface,buffer,0,0); wl_surface_damage(surface,0,0,width,height); wl_surface_commit(surface);
    for (int i=0;i<10;i++) { assert(wl_display_roundtrip(display)>=0); usleep(100000); }
    if (!manager) {
        puts("Gamescope X11 WSI PQ patches: 0/100/203/300/400/550/700/1000 nit"); fflush(stdout);
        for(int i=0;i<10;i++) {assert(wl_display_roundtrip(display)>=0);sleep(1);}
        gamescope_swapchain_destroy(swapchain);
        wl_surface_destroy(surface);
        assert(wl_display_roundtrip(display)>=0);
        XDestroyWindow(xdisplay, xwindow);
        XCloseDisplay(xdisplay);
        wl_display_disconnect(display);
        return 0;
    }
    struct wp_color_management_surface_feedback_v1 *feedback = wp_color_manager_v1_get_surface_feedback(manager, surface);
    wl_proxy_add_dispatcher((void *)feedback, ignore, NULL, NULL);
    struct wp_image_description_v1 *preferred = wp_color_management_surface_feedback_v1_get_preferred_parametric(feedback);
    wl_proxy_add_dispatcher((void *)preferred, ignore, NULL, NULL);
    while (!ready) assert(wl_display_roundtrip(display) >= 0);
    struct wp_image_description_info_v1 *information = wp_image_description_v1_get_information(preferred);
    wl_proxy_add_dispatcher((void *)information, info, NULL, NULL);
    assert(wl_display_roundtrip(display) >= 0);
    printf("preferred peak=%u reference=%u\n", peak, reference); fflush(stdout);
    if (argc > 1) assert(peak == (unsigned)atoi(argv[1]));
    unsigned content_reference = argc > 2 ? atoi(argv[2]) : reference;
    struct wp_image_description_creator_params_v1 *params = wp_color_manager_v1_create_parametric_creator(manager);
    wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_luminances(params, 0, 10000, content_reference);
    wp_image_description_creator_params_v1_set_mastering_luminance(params, 0, 550);
    wp_image_description_creator_params_v1_set_max_cll(params, 550);
    wp_image_description_creator_params_v1_set_max_fall(params, 300);
    ready = 0;
    struct wp_image_description_v1 *desc = wp_image_description_creator_params_v1_create(params);
    wl_proxy_add_dispatcher((void *)desc, ignore, NULL, NULL);
    while (!ready) assert(wl_display_roundtrip(display) >= 0);
    struct wp_color_management_surface_v1 *color = wp_color_manager_v1_get_surface(manager, surface);
    wp_color_management_surface_v1_set_image_description(color, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wl_surface_attach(surface,buffer,0,0); wl_surface_damage(surface,0,0,width,height); wl_surface_commit(surface);
    assert(wl_display_roundtrip(display)>=0);
    printf("PQ patches: ref=%u, 0/100/203/300/400/550/700/1000 nit; %d-bit input quantization\n",content_reference,depth);fflush(stdout);
    for(int i=0;i<10;i++) {assert(wl_display_roundtrip(display)>=0);sleep(1);}
    return 0;
}
