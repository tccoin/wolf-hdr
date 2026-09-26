/*
 * Small SDL2 test utility for Moonlight's DualSense adaptive-trigger path.
 * Press Cross/X or Circle/O to cycle through adaptive-trigger effects and
 * pulse the regular rumble motors. SDL maps the two face buttons to A and B
 * on a DualSense. Testing both output paths here prevents a working input
 * path from hiding a broken Moonlight feedback channel.
 * The report layout and sample effect values follow SDL's upstream test:
 * https://github.com/libsdl-org/SDL/blob/120c76c84bbce4c1bfed4e9eb74e10678bd83120/test/testgamecontroller.c
 */
#define _GNU_SOURCE
#include <SDL.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
  Uint8 enable_bits_1;
  Uint8 enable_bits_2;
  Uint8 rumble_right;
  Uint8 rumble_left;
  Uint8 headphone_volume;
  Uint8 speaker_volume;
  Uint8 microphone_volume;
  Uint8 audio_enable_bits;
  Uint8 mic_light_mode;
  Uint8 audio_mute_bits;
  Uint8 right_trigger_effect[11];
  Uint8 left_trigger_effect[11];
  Uint8 unknown_1[6];
  Uint8 led_flags;
  Uint8 unknown_2[2];
  Uint8 led_animation;
  Uint8 led_brightness;
  Uint8 pad_lights;
  Uint8 led_red;
  Uint8 led_green;
  Uint8 led_blue;
} DualSenseEffectsState;

_Static_assert(sizeof(DualSenseEffectsState) == 47, "Unexpected DualSense effect report size");

static const Uint8 trigger_effects[][11] = {
    /* Clear the effect. */
    {0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* Constant resistance across the trigger pull. */
    {0x01, 0, 110, 0, 0, 0, 0, 0, 0, 0, 0},
    /* Resistance and vibration when the trigger is pulled. */
    {0x06, 15, 63, 128, 0, 0, 0, 0, 0, 0, 0},
};

static const char *effect_names[] = {"clear", "constant resistance", "resistance + trigger rumble"};
static volatile sig_atomic_t stop_requested;

/* SDL can legitimately choose the evdev compatibility node for a virtual
 * DualSense.  evdev exposes buttons and ordinary FF rumble, but it has no API
 * for Sony's report 0x02 adaptive-trigger extension.  Keep SDL for input and
 * use the controller's own hidraw output endpoint for this one extension. */
static int open_dualsense_hidraw(void) {
  DIR *directory = opendir("/sys/class/hidraw");
  if (directory == NULL) {
    return -1;
  }

  int fd = -1;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strncmp(entry->d_name, "hidraw", 6) != 0) {
      continue;
    }
    char path[64];
    if (snprintf(path, sizeof(path), "/dev/%s", entry->d_name) >= (int)sizeof(path)) {
      continue;
    }
    const int candidate = open(path, O_WRONLY | O_CLOEXEC);
    if (candidate < 0) {
      continue;
    }
    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(candidate, HIDIOCGRAWINFO, &info) == 0 &&
        info.vendor == 0x054c && info.product == 0x0ce6) {
      fd = candidate;
      break;
    }
    close(candidate);
  }
  closedir(directory);
  return fd;
}

static void request_stop(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static int send_trigger_effect(int hidraw_fd, unsigned int effect_index) {
  DualSenseEffectsState state;
  memset(&state, 0, sizeof(state));
  state.enable_bits_1 = 0x04 | 0x08; /* Update right and left trigger effect. */
  memcpy(state.right_trigger_effect, trigger_effects[effect_index], sizeof(state.right_trigger_effect));
  memcpy(state.left_trigger_effect, trigger_effects[effect_index], sizeof(state.left_trigger_effect));

  /* Numbered output reports must include report ID 0x02 before the payload. */
  Uint8 report[sizeof(state) + 1];
  report[0] = 0x02;
  memcpy(report + 1, &state, sizeof(state));
  const ssize_t written = write(hidraw_fd, report, sizeof(report));
  if (written == (ssize_t)sizeof(report)) {
    return 0;
  }
  if (written < 0) {
    fprintf(stderr, "Could not write DualSense hidraw report: %s\n", strerror(errno));
  } else {
    fprintf(stderr, "Incomplete DualSense hidraw report: %zd of %zu bytes\n", written, sizeof(report));
  }
  return -1;
}

static SDL_GameController *open_controller(int device_index) {
  if (!SDL_IsGameController(device_index)) {
    return NULL;
  }

  SDL_GameController *controller = SDL_GameControllerOpen(device_index);
  if (controller == NULL) {
    fprintf(stderr, "Could not open controller %d: %s\n", device_index, SDL_GetError());
  }
  return controller;
}

static SDL_GameController *open_first_controller(void) {
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    SDL_GameController *controller = open_controller(i);
    if (controller != NULL) {
      return controller;
    }
  }
  return NULL;
}

static SDL_GameController *reopen_after_hotplug(void) {
  /* The virtual evdev node can become visible slightly before hidraw when a
   * lobby is rejoined.  Keeping SDL's old device list in that window makes it
   * select the evdev fallback (rumble works, SendEffect does not).  Rebuilding
   * the subsystem after udev settles makes SDL choose its DualSense HIDAPI
   * backend, which supports adaptive-trigger output reports. */
  SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
  SDL_Delay(100);
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "Could not reinitialize SDL controller support: %s\n", SDL_GetError());
    return NULL;
  }
  return open_first_controller();
}

static SDL_JoystickID activate_controller(SDL_GameController *controller, int hidraw_fd) {
  const SDL_GameControllerType type = SDL_GameControllerGetType(controller);
  printf("Controller connected: %s\n", SDL_GameControllerName(controller));
  if (type != SDL_CONTROLLER_TYPE_PS5) {
    fprintf(stderr, "Warning: SDL did not identify this controller as a DualSense (type %d).\n", type);
  }
  if (send_trigger_effect(hidraw_fd, 0) != 0) {
    fprintf(stderr, "Could not clear existing trigger effects: %s\n", SDL_GetError());
  }
  fflush(stdout);
  return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
}

int main(void) {
  if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
    return 1;
  }

  puts("\nPress Cross/X or Circle/O to cycle both triggers:");
  puts("  clear -> constant resistance -> resistance + trigger rumble -> clear");
  puts("The test keeps waiting if the controller or Moonlight stream reconnects.");
  puts("Close this window to quit.\n");
  fflush(stdout);

  signal(SIGHUP, request_stop);
  signal(SIGINT, request_stop);
  signal(SIGTERM, request_stop);

  SDL_GameController *controller = open_first_controller();
  int hidraw_fd = open_dualsense_hidraw();
  if (hidraw_fd < 0) {
    fputs("Waiting for the DualSense hidraw output endpoint...\n", stderr);
  } else {
    puts("Using the native DualSense hidraw output endpoint.");
  }
  SDL_JoystickID controller_id = -1;
  if (controller != NULL && hidraw_fd >= 0) {
    controller_id = activate_controller(controller, hidraw_fd);
  } else {
    puts("Waiting for a DualSense controller...");
    fflush(stdout);
  }

  unsigned int current_effect = 0;
  Uint64 reopen_after_ms = 0;
  bool running = true;
  while (running && !stop_requested) {
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 1000)) {
      if (stop_requested) {
        break;
      }
      /* SDL normally emits CONTROLLERDEVICEADDED.  Delay and rebuild its
       * device list so hidraw wins over the earlier evdev compatibility node. */
      if (controller == NULL) {
        const Uint64 now = SDL_GetTicks64();
        if (reopen_after_ms != 0 && now < reopen_after_ms) {
          continue;
        }
        if (hidraw_fd < 0) {
          hidraw_fd = open_dualsense_hidraw();
          if (hidraw_fd >= 0) {
            puts("Native DualSense hidraw output endpoint is ready.");
          }
        }
        controller = reopen_after_ms == 0 ? open_first_controller() : reopen_after_hotplug();
        if (controller != NULL && hidraw_fd >= 0) {
          controller_id = activate_controller(controller, hidraw_fd);
          current_effect = 0;
        }
        reopen_after_ms = 0;
      }
      continue;
    }
    if (event.type == SDL_QUIT) {
      running = false;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != NULL &&
        event.cdevice.which == controller_id) {
      puts("Controller disconnected; waiting for it to return...");
      SDL_GameControllerClose(controller);
      controller = NULL;
      controller_id = -1;
      current_effect = 0;
      reopen_after_ms = SDL_GetTicks64() + 1500;
      fflush(stdout);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED && controller == NULL) {
      /* More nodes belonging to the same DualSense may still be arriving. */
      reopen_after_ms = SDL_GetTicks64() + 1500;
      continue;
    }
    if (controller == NULL || event.type != SDL_CONTROLLERBUTTONDOWN ||
        event.cbutton.which != controller_id ||
        (event.cbutton.button != SDL_CONTROLLER_BUTTON_A && event.cbutton.button != SDL_CONTROLLER_BUTTON_B)) {
      continue;
    }

    current_effect = (current_effect + 1) % (sizeof(trigger_effects) / sizeof(trigger_effects[0]));
    const int result = send_trigger_effect(hidraw_fd, current_effect);
    if (result == 0) {
      printf("Sent to both triggers: %s\n", effect_names[current_effect]);
    } else {
      printf("Failed to send trigger effect: %s\n", SDL_GetError());
    }
    const int rumble_result = SDL_GameControllerRumble(controller, 0x8000, 0x8000, 750);
    if (rumble_result == 0) {
      puts("Sent regular rumble: 50% for 750 ms");
    } else {
      printf("Failed to send regular rumble: %s\n", SDL_GetError());
    }
    if (result != 0) {
      puts("Adaptive-trigger output report failed; waiting for the native hidraw endpoint to return...");
      close(hidraw_fd);
      hidraw_fd = -1;
    }
    fflush(stdout);
  }

  /* Best-effort cleanup so closing the utility does not leave resistance on. */
  if (controller != NULL && SDL_GameControllerGetAttached(controller) && hidraw_fd >= 0) {
    send_trigger_effect(hidraw_fd, 0);
  }
  if (controller != NULL) {
    SDL_GameControllerClose(controller);
  }
  if (hidraw_fd >= 0) {
    close(hidraw_fd);
  }
  SDL_Quit();
  return 0;
}
