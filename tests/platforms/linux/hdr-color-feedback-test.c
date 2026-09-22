/* Regression for synchronous destruction of newly-created Wayland resources.
 * Generate color-management-client.h and color-management-protocol.c from
 * color-management-v1.xml with wayland-scanner; link with wayland-client.
 * Run against an isolated Wolf compositor with WOLF_HDR_CM=1.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "color-management-client.h"

static struct wl_compositor *compositor;
static struct wp_color_manager_v1 *manager;

static int ignore_event(const void *impl, void *target, uint32_t opcode,
                        const struct wl_message *message, union wl_argument *args)
{
  return 0;
}

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
  if (!strcmp(interface, "wl_compositor"))
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
  if (!strcmp(interface, "wp_color_manager_v1")) {
    manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    wl_proxy_add_dispatcher((struct wl_proxy *) manager, ignore_event, NULL, NULL);
  }
}

static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, removed};

struct result { bool done; bool pq; bool bt2020; };

static int information(const void *impl, void *target, uint32_t opcode,
                       const struct wl_message *message, union wl_argument *args)
{
  struct result *result = wl_proxy_get_user_data(target);
  if (!strcmp(message->name, "tf_named"))
    result->pq = args[0].u == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
  if (!strcmp(message->name, "primaries_named"))
    result->bt2020 = args[0].u == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
  if (!strcmp(message->name, "done")) {
    result->done = true;
    wl_proxy_destroy(target);
  }
  return 0;
}

int main(void)
{
  struct wl_display *display = wl_display_connect(NULL);
  assert(display);
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  assert(wl_display_roundtrip(display) >= 0);
  assert(compositor && manager);
  struct wl_surface *surface = wl_compositor_create_surface(compositor);
  struct wp_color_management_surface_feedback_v1 *feedback =
      wp_color_manager_v1_get_surface_feedback(manager, surface);
  wl_proxy_add_dispatcher((struct wl_proxy *) feedback, ignore_event, NULL, NULL);
  assert(wl_display_roundtrip(display) >= 0);
  for (int i = 0; i < 1000; ++i) {
    struct result result = {0};
    struct wp_image_description_v1 *description =
        wp_color_management_surface_feedback_v1_get_preferred_parametric(feedback);
    struct wp_image_description_info_v1 *info =
        wp_image_description_v1_get_information(description);
    wl_proxy_add_dispatcher((struct wl_proxy *) info, information, NULL, &result);
    // Match KWin: request information and immediately destroy the description.
    wp_image_description_v1_destroy(description);
    while (!result.done)
      assert(wl_display_dispatch(display) >= 0);
    assert(result.pq && result.bt2020);
  }
  puts("PASS: 1000 HDR feedback create/get_information/destroy cycles (PQ/BT.2020)");
  wp_color_management_surface_feedback_v1_destroy(feedback);
  wl_surface_destroy(surface);
  wp_color_manager_v1_destroy(manager);
  wl_compositor_destroy(compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return 0;
}
