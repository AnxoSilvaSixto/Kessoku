# Build-machine-specific vcpkg triplet for the profile-guided training build.
#
# Same Zen 3 tuning as x64-windows-native, plus /GENPROFILE so every dependency
# DLL records branch counts during the training sessions. The linker writes a
# default-named .pgd next to each output image; scripts/pgo.ps1 merges the
# collected .pgc files into those databases and relinks the trained DLLs with
# /USEPROFILE. Only Release packages are needed: both PGO presets build Release.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_C_FLAGS_RELEASE   "/Ob3 /Gw /Zc:inline /arch:AVX2 /fp:fast /GL")
set(VCPKG_CXX_FLAGS_RELEASE "/Ob3 /Gw /Zc:inline /arch:AVX2 /fp:fast /GL")
set(VCPKG_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF /GENPROFILE")
