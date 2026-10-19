// SPDX-License-Identifier: GPL-3.0-or-later
#include "Init.h"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

hpl::tWString CoopPersonalDirectory() {
    hpl::tWString root;
    const wchar_t* overrideRoot = _wgetenv(L"PENUMBRA_COOP_PROFILE_ROOT");
    if (overrideRoot && *overrideRoot) root = overrideRoot;
    else root = hpl::GetSystemSpecialPath(hpl::eSystemPath_Personal);
    if (root.empty()) throw std::runtime_error("Cannot locate a directory for the cooperative profile");
    std::filesystem::create_directories(std::filesystem::path(root));
    if (root.back() != L'/' && root.back() != L'\\') root += L'/';
    return root;
}