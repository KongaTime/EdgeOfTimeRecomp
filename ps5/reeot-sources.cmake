# EdgeOfTimeRecomp for the PS5 - the sources a PS5 title builds of this repository.
#
# PS5_EdgeOfTimeTitle (reeot/reeot_game.cmake) includes this: the desktop build's
# REEOT_SOURCES (CMakeLists.txt) without what only the desktop uses, and the game
# logic module's own sources. Keep it in step with CMakeLists.txt.
#
# Left out: src/platform/file_dialog.cpp (the installer's, which the PS5 has not),
# src/installer/{disc_install,installer_music,installer_wizard}.cpp
# (REEOT_BUILD_INSTALLER), src/platform/moltenvk.cpp (macOS), src/core/sleep.cpp
# (Windows and macOS).

set(REEOT_PS5_SOURCES
    src/main.cpp
    src/reeot_app.cpp
    src/platform/desktop_shortcut.cpp
    src/platform/process.cpp
    src/platform/user_dirs.cpp
    src/platform/display.cpp
    src/platform/fatal_dialog.cpp
    src/platform/crash_handler.cpp
    src/ui/theme.cpp
    src/ui/watermark.cpp
    src/platform/update_check.cpp
    src/installer/install_registry.cpp
    src/installer/self_install.cpp
    src/installer/dlc_publish.cpp
    src/mods/mod_manager.cpp
    src/core/critical_section.cpp
    src/core/quit.cpp
    src/gpu/settings.cpp
    src/gpu/device.cpp
    src/gpu/frame_ring.cpp
    src/gpu/present.cpp
    src/gpu/imgui_overlay.cpp
    src/gpu/format.cpp
    src/gpu/textures.cpp
    src/gpu/surfaces.cpp
    src/gpu/vertex_layout.cpp
    src/gpu/draw.cpp
    src/gpu/render_thread.cpp
    src/gpu/shaders/shader_dump.cpp
    src/gpu/resolve.cpp
    src/gpu/trace.cpp
    src/gpu/pipeline/pipeline_cache.cpp
    src/gpu/gpu_timing.cpp
    src/gpu/memory_report.cpp
    src/gpu/pipeline/pso_records.cpp
    src/gpu/pipeline/pso_precache.cpp
    src/gpu/pipeline/pso_predictor.cpp
    src/gpu/hooks/package_load.cpp
    src/gpu/patches/update_rate.cpp
    src/gpu/patches/aspect_ratio.cpp
    src/gpu/patches/post_effects.cpp
    src/goliath/controller/pad_remap.cpp
    src/goliath/controller/bind_capture.cpp
    src/goliath/ui/achievement_feed.cpp
    src/goliath/controller/button_glyphs.cpp
    src/goliath/controller/mouse_input.cpp
    src/goliath/debug/freecam.cpp
    src/goliath/ui/aspect_policy.cpp
    src/goliath/ui/overlays/fps.cpp
    src/goliath/loading/reeot_package.cpp
    src/goliath/loading/texture_overrides.cpp
    src/goliath/web_color.cpp
    src/goliath/text/glyph_pages.cpp
    src/gpu/taa.cpp
    src/gpu/shaders/guest_shaders.cpp
    src/gpu/hooks/stubs.cpp
    src/gpu/hooks/state.cpp
    src/gpu/hooks/resource.cpp
)

# Built into the game logic module (reeot_GameLogic), which a title links in
set(REEOT_PS5_GAMELOGIC_SOURCES
    src/gamelogic/debug/debug_mode.cpp
    src/gamelogic/patches/game_patches.cpp
    src/gamelogic/ui/options_pages.cpp
    src/gamelogic/ui/achievements_page.cpp
    src/gamelogic/input/input.cpp
)

# The host shaders (src/gpu/shaders/hlsl), as CMakeLists.txt compiles them
set(REEOT_PS5_HOST_SHADERS
    copy_vs:vs_6_0 imgui_vs:vs_6_0
    blit_ps:ps_6_0 copy_depth_ps:ps_6_0 imgui_ps:ps_6_0 taa_ps:ps_6_0
    velocity_resolve_2x_ps:ps_6_0 velocity_resolve_4x_ps:ps_6_0 velocity_resolve_8x_ps:ps_6_0
    resolve_msaa_color_2x_ps:ps_6_0 resolve_msaa_color_4x_ps:ps_6_0 resolve_msaa_color_8x_ps:ps_6_0
    resolve_msaa_depth_2x_ps:ps_6_0 resolve_msaa_depth_4x_ps:ps_6_0 resolve_msaa_depth_8x_ps:ps_6_0
    derive_depth_stencil_2x_ps:ps_6_0 derive_depth_stencil_4x_ps:ps_6_0 derive_depth_stencil_8x_ps:ps_6_0
)
