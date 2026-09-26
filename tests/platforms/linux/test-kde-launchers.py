#!/usr/bin/env python3
"""Source regression guards for portable desktop launchers."""
import configparser
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DesktopLaunchers(unittest.TestCase):
    def test_kde_entrypoint_has_valid_shell_syntax(self):
        subprocess.run(
            ['bash', '-n', str(ROOT / 'docker/wolf-selkies-kwin-entrypoint.sh')],
            check=True,
        )

    def test_english_default_labels(self):
        files = list((ROOT / 'docker/kde').glob('*.desktop'))
        self.assertEqual({path.name for path in files}, {
            'dlssnr.desktop',
            'dualsense-trigger-test.desktop',
            'heroic.desktop',
            'return-to-wolf-ui.desktop',
            'steam-big-picture.desktop',
            'steam.desktop',
        })
        for path in files:
            config = configparser.ConfigParser(interpolation=None)
            config.read(path)
            for section in config.values():
                for key in ('Name', 'Comment'):
                    if key in section:
                        self.assertTrue(section[key].isascii(), f'{path.name}: {key}')

    def test_default_config_seeds_kde_and_big_picture_tiles(self):
        default_config = (ROOT / 'src/moonlight-server/state/default/config.include.toml').read_text()
        self.assertIn('title = "Steam (KDE)"', default_config)
        self.assertIn('title = "KDE"', default_config)
        self.assertLess(default_config.index('title = "Steam (KDE)"'),
                        default_config.index('title = "KDE"'))
        self.assertIn('WOLF_KDE_STEAM_MODE=big-picture', default_config)
        self.assertIn('image = "wolf-kde:hdr"', default_config)

    def test_dualsense_trigger_test_is_built_and_installed_on_desktop(self):
        dockerfile = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        self.assertIn('tools/dualsense-trigger-test.c', dockerfile)
        self.assertIn('/usr/local/bin/wolf-dualsense-trigger-test', dockerfile)
        self.assertIn('/usr/local/share/wolf/dualsense-trigger-test.desktop', dockerfile)

        entrypoint = (ROOT / 'docker/wolf-selkies-kwin-entrypoint.sh').read_text()
        self.assertIn('dualsense-trigger-test.desktop "$HOME/Desktop/dualsense-trigger-test.desktop"', entrypoint)

        launcher = (ROOT / 'docker/kde/dualsense-trigger-test.desktop').read_text()
        self.assertIn('xterm -hold -title', launcher)
        self.assertIn('wolf-dualsense-trigger-test', launcher)

        source = (ROOT / 'tools/dualsense-trigger-test.c').read_text()
        self.assertIn('SDL_CONTROLLERDEVICEADDED', source)
        self.assertIn('Controller disconnected; waiting for it to return', source)
        self.assertIn('SDL_WaitEventTimeout', source)
        self.assertIn('reopen_after_hotplug', source)
        self.assertIn('SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER)', source)
        self.assertIn("SDL choose its DualSense HIDAPI", source)
        self.assertNotIn('No SDL game controller found. Connect the DualSense and relaunch this test.', source)

    def test_return_to_wolf_ui_launcher_only_stops_the_kde_runner(self):
        dockerfile = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        self.assertIn('wolf-return-to-ui.sh /usr/local/bin/wolf-return-to-ui', dockerfile)
        self.assertIn('return-to-wolf-ui.desktop /usr/local/share/wolf/return-to-wolf-ui.desktop', dockerfile)

        entrypoint = (ROOT / 'docker/wolf-selkies-kwin-entrypoint.sh').read_text()
        self.assertIn('return-to-wolf-ui.desktop "$HOME/Desktop/return-to-wolf-ui.desktop"', entrypoint)

        launcher = (ROOT / 'docker/wolf-return-to-ui.sh').read_text()
        self.assertIn('kill -TERM 1', launcher)
        self.assertNotIn('curl ', launcher)
        self.assertNotIn('service/restart', launcher)

    def test_dualsense_effect_feedback_uses_serialized_sequenced_enet(self):
        input_handler = (ROOT / 'src/moonlight-server/control/input_handler.cpp').read_text()
        self.assertIn('set_on_trigger_effect(on_adaptive_trigger_fn)', input_handler)
        self.assertIn('ControlAdaptiveTriggerPacket', input_handler)
        self.assertIn('std::is_base_of_v<inputtino::PS5Joypad', input_handler)
        self.assertNotIn('std::is_same_v<std::decay_t<decltype(pad)>, inputtino::PS5Joypad>', input_handler)
        self.assertIn('Reusing persistent virtual controller', input_handler)
        self.assertIn('Neutralizing persistent joypad', input_handler)
        self.assertNotIn('Removing joypad {}', input_handler)

        control = (ROOT / 'src/moonlight-server/control/control.cpp').read_text()
        self.assertIn('next_packet_sequences[enet_client.get()]', control)
        self.assertIn('sequence++, payload', control)
        self.assertIn('send_pending_packets();', control)
        self.assertIn('std::unordered_map<std::string, std::uint16_t> next_haptic_audio_sequences', control)
        self.assertIn('next_haptic_audio_sequences[haptic_session_id]++', control)
        self.assertNotIn('next_haptic_audio_sequences.erase(event.peer)', control)

        # A transient ENet transport disconnect must not make the running
        # game observe a udev remove/add cycle for its ScePad endpoint.  The
        # explicit Moonlight TERMINATION message remains the exit path.
        disconnect_start = control.index('case ENET_EVENT_TYPE_DISCONNECT:')
        receive_start = control.index('case ENET_EVENT_TYPE_RECEIVE:', disconnect_start)
        disconnect_case = control[disconnect_start:receive_start]
        self.assertIn('preserving session {} and its virtual devices across client reconnect', disconnect_case)
        self.assertIn('discard_pending_packets_for_peer(event.peer);', disconnect_case)
        self.assertNotIn('immer::box<PauseStreamEvent>', disconnect_case)
        self.assertIn('std::erase_if(pending_packets', control)
        self.assertIn('ClientStopLobbyComboEvent', input_handler)
        self.assertIn('moonlight_key == 0x41', input_handler)
        self.assertNotIn('moonlight_key == 0x51', input_handler)
        self.assertIn('DPAD_DOWN', input_handler)
        lobbies = (ROOT / 'src/moonlight-server/sessions/lobbies.cpp').read_text()
        self.assertIn('ClientStopLobbyComboEvent', lobbies)
        self.assertIn('StopStreamEvent', lobbies)
        self.assertGreaterEqual(lobbies.count('IDRRequestEvent'), 2)
        termination_start = control.index('if (sub_type == TERMINATION)')
        self.assertIn('PauseStreamEvent', control[termination_start:termination_start + 300])

        endpoints = (ROOT / 'src/moonlight-server/rest/endpoints.hpp').read_text()
        cancel_start = endpoints.index('void cancel(')
        cancel_case = endpoints[cancel_start:]
        self.assertIn('Stopping standalone Wolf UI session {} on /cancel', cancel_case)
        self.assertIn('if (!state::get_lobby_by_connected_session', cancel_case)
        self.assertIn('StopStreamEvent', cancel_case)
        self.assertIn('Deferring cancel cleanup for session {} by {} seconds', cancel_case)
        self.assertNotIn('StopStreamEvent>(events::StopStreamEvent',
                         cancel_case[:cancel_case.index('std::thread(')])
        self.assertIn('StopClientTransportEvent',
                      cancel_case[:cancel_case.index('std::thread(')])
        self.assertIn('Reconnect grace elapsed; stopping abandoned session {}', cancel_case)
        self.assertIn('detail::cancel_reconnect_grace(old_session->session_id)', endpoints)

        streaming = (ROOT / 'src/moonlight-server/streaming/streaming.cpp').read_text()
        self.assertEqual(4, streaming.count('StopClientTransportEvent'))

    def test_proton_does_not_inherit_appliance_system_bus(self):
        for name in ('wolf-kde-heroic.sh', 'wolf-kde-steam.sh'):
            text = (ROOT / 'docker' / name).read_text()
            cleanup = text.index('unset DBUS_SYSTEM_BUS_ADDRESS')
            first_launch = text.index('/opt/Heroic/heroic') if 'heroic' in name else text.index('"$steam"')
            self.assertLess(cleanup, first_launch)
            self.assertNotIn('unset DBUS_SESSION_BUS_ADDRESS', text)
            self.assertIn('export BWRAP=/usr/bin/bwrap', text)

    def test_heroic_shared_mount_is_visible_to_proton(self):
        text = (ROOT / 'docker/wolf-kde-heroic.sh').read_text()
        self.assertIn('WOLF_HEROIC_SHARED_HOME:-/var/lib/wolf-shared-heroic', text)
        self.assertIn('export PRESSURE_VESSEL_FILESYSTEMS_RW=', text)
        self.assertLess(text.index('export PRESSURE_VESSEL_FILESYSTEMS_RW='),
                        text.index('/opt/Heroic/heroic'))

    def test_only_games_use_gamescope(self):
        launcher = (ROOT / 'docker/wolf-kde-heroic.sh').read_text()
        game = (ROOT / 'docker/wolf-heroic-game.sh').read_text()
        self.assertNotIn('/usr/games/gamescope', launcher)
        self.assertIn('heroic-game-defaults.py', launcher)
        self.assertLess(launcher.index('flock -n 9'), launcher.index('heroic-game-defaults.py'))
        self.assertIn('exec /usr/games/gamescope --backend wayland -f --hdr-enabled', game)
        self.assertNotIn('wayland -e', game)
        self.assertIn('VK_INSTANCE_LAYERS=VK_LAYER_FROG_gamescope_wsi_x86_64 "$@"', game)

    def test_steam_client_is_not_wrapped_in_an_outer_user_namespace(self):
        steam = (ROOT / 'docker/wolf-kde-steam.sh').read_text()
        self.assertNotIn('unshare --user', steam)
        self.assertIn('"$steam" -nobigpicture', steam)
        self.assertIn('"$steam" -gamepadui', steam)

    def test_wine_preflight_gets_hdr_before_game_wrapper(self):
        launcher = (ROOT / 'docker/wolf-kde-heroic.sh').read_text()
        early = launcher.index('export DXVK_HDR=1 PROTON_ENABLE_HDR=1')
        self.assertLess(launcher.index('unset PROTON_ENABLE_HDR DXVK_HDR'), early)
        self.assertLess(early, launcher.rindex('/opt/Heroic/heroic'))
        self.assertNotIn('export VK_INSTANCE_LAYERS=', launcher)
        steam = (ROOT / 'docker/wolf-kde-steam.sh').read_text()
        self.assertLess(steam.index('export PROTON_ENABLE_HDR=1 DXVK_HDR=1'),
                        steam.index('/usr/games/gamescope "${gamescope_debug_focus_args[@]}" --backend'))
        self.assertLess(steam.index('export PROTON_ENABLE_HDR=1 DXVK_HDR=1'),
                        steam.index('"$steam" -nobigpicture'))

    def test_desktop_steam_installs_global_game_defaults_only_before_start(self):
        steam = (ROOT / 'docker/wolf-kde-steam.sh').read_text()
        install = steam.index('steam-game-defaults.py')
        self.assertIn('[ -z "$pid" ] && [ "$requested_mode" != big-picture ]', steam)
        self.assertLess(steam.index('if [ "$return_to_desktop" = 1 ]'), install)
        self.assertLess(install, steam.index('forward_existing()'))
        game = (ROOT / 'docker/wolf-steam-game.sh').read_text()
        self.assertIn('GAMESCOPE_WAYLAND_DISPLAY', game)
        self.assertIn('WOLF_KDE_STEAM_MODE:-', game)
        self.assertIn('exec /usr/games/gamescope --backend wayland -f --hdr-enabled', game)
        self.assertNotIn('wayland -e', game)
        self.assertIn('game_appid="${2}"', game)
        self.assertNotIn('nodxr', game)
        self.assertIn('export PROTON_ENABLE_NVAPI=1', game)
        self.assertIn('export PROTON_ENABLE_NGX_UPDATER=1', game)
        self.assertIn('export DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE=on', game)
        self.assertIn('export DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE=on', game)
        self.assertIn('export DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE=on', game)
        self.assertIn('export WOLF_SCEPAD_FORCE_PULSE=1', game)
        self.assertIn('export PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT=1', game)
        self.assertIn('export PROTON_SONY_WINDOWS_DEVICE_NAMES=1', game)

        heroic_game = (ROOT / 'docker/wolf-heroic-game.sh').read_text()
        self.assertIn('export WOLF_SCEPAD_FORCE_PULSE=1', heroic_game)
        self.assertIn('export PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT=1', heroic_game)
        self.assertIn('export PROTON_SONY_WINDOWS_DEVICE_NAMES=1', heroic_game)
        self.assertIn('for input_path in /dev/input /dev/uinput /dev/hidraw*;', heroic_game)
        self.assertIn('PRESSURE_VESSEL_FILESYSTEMS_RO', heroic_game)

    def test_proton_scepad_build_keeps_streamed_haptics_on_pulse(self):
        build = (ROOT / 'scripts/build-proton-scepad.sh').read_text()
        self.assertIn('wolf-scepad-standard-quad-map.patch', build)
        self.assertIn('wolf-scepad-force-pulse-route.patch', build)
        self.assertIn('force_wolf_scepad_pulse_route', build)

        standard_map = (ROOT / 'third_party/wine-scepad/patches/'
                        'wolf-scepad-standard-quad-map.patch').read_text()
        self.assertIn('PA_CHANNEL_POSITION_REAR_LEFT', standard_map)
        self.assertIn('PA_CHANNEL_POSITION_REAR_RIGHT', standard_map)

        force_route = (ROOT / 'third_party/wine-scepad/patches/'
                       'wolf-scepad-force-pulse-route.patch').read_text()
        self.assertIn('WOLF_SCEPAD_FORCE_PULSE', force_route)
        self.assertIn('use_raw_dualsense_haptic_target', force_route)

    def test_kde_steam_recovers_stale_launcher_locks_without_an_outer_namespace(self):
        steam = (ROOT / 'docker/wolf-kde-steam.sh').read_text()
        self.assertIn('regenerable files owned by root', steam)
        self.assertIn('exec 8<"$transition_lock"', steam)
        self.assertIn('exec 9<"$startup_lock"', steam)
        self.assertNotIn('unshare --user', steam)

    def test_kde_lobby_audio_producer_exists_before_runner_startup(self):
        lobbies = (ROOT / 'src/moonlight-server/sessions/lobbies.cpp').read_text()
        audio_setup = lobbies.index('[LOBBY] Create audio virtual sink')
        runner_setup = lobbies.index('[LOBBY] Start runner')
        self.assertLess(audio_setup, runner_setup)
        self.assertIn('v_device->sink_idx.get();', lobbies[audio_setup:runner_setup])
        self.assertIn('streaming::start_audio_producer', lobbies[audio_setup:runner_setup])
        self.assertIn('std::thread([lobby, audio_server = audio_server->server, v_device, ev_bus, channel_count]()',
                      lobbies[audio_setup:runner_setup])

        pulse = (ROOT / 'src/core/src/platforms/linux/pulseaudio/pulse.cpp').read_text()
        self.assertIn('device.description=Wolf_Stream_Audio', pulse)

    def test_gamescope_keeps_active_pointer_constraints_for_nested_games(self):
        dockerfile = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        self.assertIn('wayland-restore-active-pointer-constraint', dockerfile)

        patch = (ROOT / 'docker/patches/'
                 'gamescope-wayland-restore-active-pointer-constraint.patch').read_text()
        self.assertIn('m_bConstrained = !!pConstraint;', patch)
        self.assertNotIn('+\t\tm_bConstrained = false;', patch)

    def test_desktop_session_preserves_steam_input_reconnect_and_overlay(self):
        steam = (ROOT / 'docker/wolf-kde-steam.sh').read_text()
        big_picture = steam[steam.index('if [ "$WOLF_KDE_STEAM_MODE" = big-picture ]'):]
        self.assertIn('export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP=1', big_picture)
        self.assertIn('/usr/games/gamescope "${gamescope_debug_focus_args[@]}" --backend wayland -e -f --hdr-enabled', big_picture)
        self.assertIn('WOLF_KDE_GAMESCOPE_DEBUG_FOCUS', big_picture)

        entrypoint = (ROOT / 'docker/wolf-selkies-kwin-entrypoint.sh').read_text()
        self.assertIn('if [ -c /dev/uinput ]; then', entrypoint)
        self.assertIn('usermod -a -G "$uinput_group" ubuntu', entrypoint)
        self.assertIn('wolf-uinput-$uinput_gid', entrypoint)
        self.assertIn('steam_mode="${WOLF_KDE_STEAM_MODE:-big-picture}"', entrypoint)
        self.assertIn('/usr/bin/steam --wolf-big-picture', entrypoint)

        game = (ROOT / 'docker/wolf-steam-game.sh').read_text()
        self.assertIn('export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP="${WOLF_GAMESCOPE_WSI_OVERLAY_BOOTSTRAP:-1}"', game)

        lobbies = (ROOT / 'src/moonlight-server/sessions/lobbies.cpp').read_text()
        self.assertIn('already_connected', lobbies)
        self.assertIn('preserving input device identities', lobbies)
        self.assertNotIn('schedule_single_player_pause_cleanup', lobbies)
        self.assertIn('on_moonlight_session_over(pause_stream_event->session_id)', lobbies)

        focus_patch = (ROOT / 'docker/patches/gamescope-overlay-focus-restore.patch').read_text()
        self.assertIn('GAMESCOPECTRL_BASELAYER_APPID', focus_patch)
        self.assertIn('SingleApplication && !ctxFocusControlAppIDs.empty()', focus_patch)
        self.assertIn('ICCCM_NORMAL_STATE', focus_patch)
        self.assertIn('XGetInputFocus( ctx->dpy, &actualKeyboardFocus', focus_patch)
        self.assertIn('keyboardFocusDrifted', focus_patch)
        dockerfile = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        self.assertIn('overlay-focus-restore', dockerfile)

    def test_global_and_nested_kde_hdr_peaks_remain_independent(self):
        sessions = (ROOT / 'src/moonlight-server/sessions/common.cpp').read_text()
        self.assertIn('full_env.set("WOLF_HDR_PEAK_NITS"', sessions)

        entrypoint = (ROOT / 'docker/wolf-selkies-kwin-entrypoint.sh').read_text()
        self.assertIn('KWIN_WAYLAND_HDR_PEAK_NITS="${WOLF_KDE_HDR_PEAK_NITS:-0}"', entrypoint)
        self.assertNotIn('output.WL-0.maxBrightnessOverride.', entrypoint)
        self.assertNotIn('hdr_peak="${WOLF_HDR_PEAK_NITS:-1000}"', entrypoint)

        edid_patch = (ROOT / 'docker/patches/gamescope-nested-hdr-edid.patch').read_text()
        self.assertIn('getenv( "WOLF_HDR_PEAK_NITS" )', edid_patch)
        self.assertIn('maxPeakLuminance = uint32_t( parsed )', edid_patch)
        self.assertIn('std::min<uint32_t>( hdrInfo.uMaxFrameAverageLuminance, maxPeakLuminance )', edid_patch)

    def test_kde_sdr_to_pq_frames_are_complete_before_nvenc(self):
        bridge = (ROOT / 'third_party/gst-wayland-display/gst-plugin-wayland-display'
                  / 'src/waylandsrc/hdr_gl.c').read_text()
        transform = bridge.index('gl->DrawArrays (GL_TRIANGLES, 0, 3);')
        finish = bridge.index('gl->Finish ();', transform)
        handoff = bridge.index('job->texture = output_texture;', transform)
        self.assertLess(transform, finish)
        self.assertLess(finish, handoff)

    def test_hdr_gamescope_promotes_only_hdr_session_10bit_srgb(self):
        patch = (ROOT / 'docker/patches/gamescope-wsi-auto-hdr10.patch').read_text()
        self.assertIn('gamescopeSurface->shouldExposeHDR()', patch)
        self.assertIn('VK_FORMAT_A2B10G10R10_UNORM_PACK32', patch)
        self.assertIn('VK_FORMAT_A2R10G10B10_UNORM_PACK32', patch)
        self.assertIn('VK_COLOR_SPACE_SRGB_NONLINEAR_KHR', patch)
        self.assertIn('VK_COLOR_SPACE_HDR10_ST2084_EXT', patch)
        self.assertNotIn('game_appid', patch)

        dockerfile = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        self.assertIn('wsi-auto-hdr10', dockerfile)


if __name__ == '__main__':
    unittest.main()
