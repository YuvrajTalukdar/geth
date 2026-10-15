// Unit tests for the UI short-name helpers (util/names.hpp): the strings the
// header and panel titles show, derived from the hardware's full model names.

#include <doctest.h>

#include "util/names.hpp"

using namespace geth;

TEST_SUITE("names") {

TEST_CASE("short_cpu: vendor prefixes and the graphics suffix") {
  CHECK(names::short_cpu("AMD Ryzen 5 5600H with Radeon Graphics") == "Ryzen 5 5600H");
  CHECK(names::short_cpu("AMD Ryzen 7 260 with Radeon Graphics") == "Ryzen 7 260");
  // The newer APUs report the iGPU model in the suffix too — still dropped.
  CHECK(names::short_cpu("AMD Ryzen 7 260 with Radeon 880M Graphics") == "Ryzen 7 260");
  // No suffixes at all.
  CHECK(names::short_cpu("AMD Ryzen 7 5800X") == "Ryzen 7 5800X");
  // Trailing "NN-Core Processor".
  CHECK(names::short_cpu("AMD Ryzen 9 7945HX 16-Core Processor") == "Ryzen 9 7945HX");
  // Intel: both the (R) and (TM) markers are stripped.
  CHECK(names::short_cpu("Intel(R) Core(TM) i5-1340P") == "i5-1340P");
}

TEST_CASE("short_cpu: blank input") {
  CHECK(names::short_cpu("") == "");
  CHECK(names::short_cpu("   ") == "");
}

TEST_CASE("short_gpu: NVIDIA names") {
  // "NVIDIA " and "GeForce " are dropped; a trailing " GPU" too.
  CHECK(names::short_gpu("NVIDIA GeForce RTX 3050 Laptop GPU", false, "") == "RTX 3050 Laptop");
  CHECK(names::short_gpu("NVIDIA GeForce RTX 5060 Laptop GPU", false, "") == "RTX 5060 Laptop");
  // No " GPU" suffix to strip.
  CHECK(names::short_gpu("NVIDIA GeForce RTX 3070", false, "") == "RTX 3070");
  // A non-GeForce NVIDIA card: only the "NVIDIA " prefix is dropped.
  CHECK(names::short_gpu("NVIDIA A100 80GB PCIe", false, "") == "A100 80GB PCIe");
  // NVML (and some pci.ids entries) omit the "NVIDIA " prefix entirely — the
  // "GeForce " prefix alone must still trigger the NVIDIA branch.
  CHECK(names::short_gpu("GeForce RTX 3050 Mobile", false, "") == "RTX 3050 Mobile");
  // No NVML name (session not open yet): pci.ids may hand us the chip
  // codename + bracket, "GA107M [GeForce RTX 3050 Mobile]". The bracket
  // fallback recovers the product name; the NVML name replaces it later.
  CHECK(names::short_gpu("GA107M [GeForce RTX 3050 Mobile]", false, "") ==
        "GeForce RTX 3050 Mobile");
}

TEST_CASE("short_gpu: AMD integrated (codename before the bracket + iGPU)") {
  CHECK(names::short_gpu("Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]", true, "") ==
        "Cezanne iGPU");
  CHECK(names::short_gpu("Phoenix [Radeon Graphics]", true, "") == "Phoenix iGPU");
  // No bracket at all: the whole (trimmed) name is the codename.
  CHECK(names::short_gpu("Cezanne", true, "") == "Cezanne iGPU");
}

TEST_CASE("short_gpu: AMD discrete (product_name first, else the bracket)") {
  // The amdgpu product_name wins when present…
  CHECK(names::short_gpu("Navi 22 [Radeon RX 6700/6700 XT / Navi 22]", false, "Radeon RX 6700 XT") ==
        "Radeon RX 6700 XT");
  // …and a leading "AMD " on it is stripped.
  CHECK(names::short_gpu("Navi 21 [Radeon RX 6800 / 6800 XT]", false, "AMD Radeon RX 6800") ==
        "Radeon RX 6800");
  // No product_name (older kernel): the first "/"-separated alternative inside
  // the bracket.
  CHECK(names::short_gpu("Navi 22 [Radeon RX 6700/6700 XT]", false, "") == "Radeon RX 6700");
}

TEST_CASE("short_gpu: blank input") {
  CHECK(names::short_gpu("", false, "") == "");
  CHECK(names::short_gpu("   ", false, "") == "");
}

}  // TEST_SUITE
