# Build-machine-specific vcpkg triplet for Kessoku.
#
# Tuned for the local AMD Ryzen 7 5700X (Zen 3). AVX2 is the highest ISA level
# that CPU supports, and /arch:AVX2 also unlocks FMA and BMI. Only the Release
# compiler and linker flags are strengthened; Debug dependencies stay fully
# debuggable. All of these flags match the first-party profile in
# CMakeLists.txt, so the app and its decoder/tagging libraries are built for
# the same machine.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)

set(VCPKG_C_FLAGS_RELEASE   "/Ob3 /Gw /Zc:inline /arch:AVX2 /fp:fast /GL")
set(VCPKG_CXX_FLAGS_RELEASE "/Ob3 /Gw /Zc:inline /arch:AVX2 /fp:fast /GL")
set(VCPKG_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF")
