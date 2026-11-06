#pragma once

#include <guiddef.h>

namespace contour::apo {

// Existing global-registration identities. Keep these stable so installed
// compatibility packages and their recovery journals remain usable.
inline constexpr CLSID pre_mix_clsid = {
    0xd1f3f732, 0xfd5b, 0x48ab, {0xa9, 0x89, 0x50, 0x86, 0xb6, 0xfb, 0xea, 0x37}};
inline constexpr CLSID post_mix_clsid = {
    0xe841587d, 0xe457, 0x4f68, {0xa4, 0x25, 0xc9, 0x89, 0xa1, 0xdd, 0x8c, 0xff}};

// Device-relative (HKR) registration must use different COM class identities.
// Registration scope is independent of placement: these still implement the
// tested LFX/GFX contract, not SFX/MFX/EFX.
inline constexpr CLSID component_pre_mix_clsid = {
    0xc96ce6c1, 0x7f62, 0x46a5, {0xbd, 0xff, 0x46, 0xfc, 0xa1, 0x95, 0x67, 0xab}};
inline constexpr CLSID component_post_mix_clsid = {
    0xc2db4b2b, 0x8b4d, 0x43c2, {0xbc, 0xc3, 0xa2, 0xb0, 0xd3, 0xe8, 0xfb, 0xa1}};
// A private component ID, not a physical adapter hardware ID. A future
// driver-specific extension must create this child under the audio adapter.
inline constexpr wchar_t component_id[] = L"CONTOUR&CID_37FB7288B0704FE18F53923106DAAFAB";

} // namespace contour::apo
