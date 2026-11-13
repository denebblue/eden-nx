# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Self-update helper (Windows only): swaps the installed files once the emulator has exited.
add_executable(nx_updater WIN32 main.cpp)
set_target_properties(nx_updater PROPERTIES OUTPUT_NAME "eden-nx-updater")
target_compile_definitions(nx_updater PRIVATE UNICODE _UNICODE)
target_link_libraries(nx_updater PRIVATE shell32)
