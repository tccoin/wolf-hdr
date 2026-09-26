/* Query the Windows API used by games, not merely Vulkan format support.
 * Build with MinGW; run with the game's Wine/Proton and expect 0 (SDR) or 1 (HDR).
 */
#define _WIN32_WINNT 0x0a00
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <dxgi1_6.h>

static void print_dxgi_output_luminance(void) {
  IDXGIFactory1 *factory = NULL;
  if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
    return;
  for (UINT adapter_index = 0; ; ++adapter_index) {
    IDXGIAdapter1 *adapter = NULL;
    if (FAILED(IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter)))
      break;
    for (UINT output_index = 0; ; ++output_index) {
      IDXGIOutput *output = NULL;
      IDXGIOutput6 *output6 = NULL;
      DXGI_OUTPUT_DESC1 desc;
      if (FAILED(IDXGIAdapter1_EnumOutputs(adapter, output_index, &output)))
        break;
      if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6))) {
        if (SUCCEEDED(IDXGIOutput6_GetDesc1(output6, &desc)))
          printf("DXGI output %u/%u: colorspace=%u min=%.2f max=%.2f full-frame=%.2f nits\n",
                 adapter_index, output_index, (unsigned)desc.ColorSpace,
                 desc.MinLuminance, desc.MaxLuminance, desc.MaxFullFrameLuminance);
        IDXGIOutput6_Release(output6);
      }
      IDXGIOutput_Release(output);
    }
    IDXGIAdapter1_Release(adapter);
  }
  IDXGIFactory1_Release(factory);
}

int main(int argc, char **argv) {
  /* Some Proton versions detach console programs; an optional report file
   * lets the harness verify execution instead of trusting the launcher exit. */
  if (argc == 3 && !freopen(argv[2], "w", stdout))
    return 2;
  UINT32 paths_count = 0, modes_count = 0;
  LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_count, &modes_count);
  if ((argc != 2 && argc != 3) || status != ERROR_SUCCESS || !paths_count) {
    fprintf(stderr, "HDR probe: arguments/display enumeration failed (%ld)\n", status);
    return 2;
  }
  DISPLAYCONFIG_PATH_INFO *paths = calloc(paths_count, sizeof(*paths));
  DISPLAYCONFIG_MODE_INFO *modes = calloc(modes_count, sizeof(*modes));
  if (!paths || !modes)
    return 2;
  status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_count, paths, &modes_count, modes, NULL);
  int found_hdr = 0;
  if (status == ERROR_SUCCESS) {
    for (UINT32 i = 0; i < paths_count; ++i) {
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {0};
      color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      color.header.size = sizeof(color);
      color.header.adapterId = paths[i].targetInfo.adapterId;
      color.header.id = paths[i].targetInfo.id;
      LONG result = DisplayConfigGetDeviceInfo(&color.header);
      printf("Windows HDR output %u: status=%ld supported=%u enabled=%u bits=%u\n",
             i,
             result,
             color.advancedColorSupported,
             color.advancedColorEnabled,
             color.bitsPerColorChannel);
      if (result != ERROR_SUCCESS)
        status = result;
      found_hdr |= result == ERROR_SUCCESS && color.advancedColorSupported && color.advancedColorEnabled;
    }
  }
  free(paths);
  free(modes);
  print_dxgi_output_luminance();
  return status == ERROR_SUCCESS && found_hdr == atoi(argv[1]) ? 0 : 1;
}
