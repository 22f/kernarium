// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumHostFeatures.hpp"
#include "android_boot.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kernarium::hostfeatures;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::string_view bootDevices =
    "androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1";

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr);
    require(std::fwrite(text.data(), 1, text.size(), file) == text.size());
    require(std::fclose(file) == 0);
}

void everyBitIsOneBitOfItsOwn()
{
    require(distinctRows(features));
    std::uint32_t seen = 0;
    for (const Feature& feature : features) {
        require(singleBit(feature.bit), std::string {feature.name});
        require((seen & feature.bit) == 0, std::string {feature.name});
        seen |= feature.bit;
    }
    require(seen == knownBits());
    /* The enum of the public header is what the applications state. */
    require(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS == 1U);
    require((knownBits() & KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) != 0U);
}

void aCollisionBetweenTwoRowsIsDetected()
{
    const Feature first {"first", 1U << 0, "androidboot.first=", "on"};
    const Feature second {"second", 1U << 1, "androidboot.second=", "on"};
    require(distinctRows(std::array {first, second}));

    /* The same bit twice would turn one feature into the other. */
    Feature sameBit = second;
    sameBit.bit = first.bit;
    require(!distinctRows(std::array {first, sameBit}));
    /* So would the same name or the same argument. */
    Feature sameName = second;
    sameName.name = first.name;
    require(!distinctRows(std::array {first, sameName}));
    Feature samePrefix = second;
    samePrefix.argumentPrefix = first.argumentPrefix;
    require(!distinctRows(std::array {first, samePrefix}));

    /* A row is one bit, with an argument of the form name=value. */
    Feature noBit = second;
    noBit.bit = 0;
    require(!distinctRows(std::array {first, noBit}));
    Feature twoBits = second;
    twoBits.bit = 0b110;
    require(!distinctRows(std::array {first, twoBits}));
    Feature noEquals = second;
    noEquals.argumentPrefix = "androidboot.second";
    require(!distinctRows(std::array {first, noEquals}));
    Feature noValue = second;
    noValue.argumentValue = "";
    require(!distinctRows(std::array {first, noValue}));
    Feature noName = second;
    noName.name = "";
    require(!distinctRows(std::array {first, noName}));
}

void theHapticsBitBecomesTheSkuArgument()
{
    require(kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            std::vector<std::string> {
                "androidboot.product.hardware.sku=haptics"});
    /* A device without haptics passes nothing. */
    require(kernelArguments(0).empty());
}

void everyKnownBitMakesOneArgumentInTableOrder()
{
    const auto arguments = kernelArguments(knownBits());
    require(arguments.size() == std::size(features));
    for (std::size_t row = 0; row < arguments.size(); ++row) {
        require(arguments[row] ==
                std::string {features[row].argumentPrefix} +
                    std::string {features[row].argumentValue});
    }
    /* A bit alone makes its own argument and no other. */
    for (const Feature& feature : features) {
        const auto alone = kernelArguments(feature.bit);
        require(alone.size() == 1);
        require(alone.front().starts_with(feature.argumentPrefix));
    }
}

void bitsTheRuntimeHasNoRowForAreReportedAndMakeNothing()
{
    require(unknownBits(0) == 0);
    require(unknownBits(knownBits()) == 0);
    constexpr std::uint32_t beyond = 1U << 31;
    require(unknownBits(beyond | KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            beyond);
    require(kernelArguments(beyond).empty());
    /* The unknown part does not change what the known part makes. */
    require(kernelArguments(beyond | KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) ==
            kernelArguments(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS));
}

void aReleaseCommandLineCannotSetAFeatureArgument()
{
    const uml::testing::ScratchSubdirectory scratch {"host-features-XXXXXX"};
    const auto path = scratch.path() / "command-line.txt";
    const auto prefixes = launcherPrefixes();
    require(prefixes.size() == std::size(features));

    writeText(path, "quiet " + std::string {bootDevices} + "\n");
    require(uml::android::readKernelCommandLine(path, prefixes) ==
            std::vector<std::string> {"quiet", std::string {bootDevices}});
    for (const Feature& feature : features) {
        const std::array<std::string_view, 2> values {
            feature.argumentValue, "other"};
        for (const std::string_view value : values) {
            writeText(path, "quiet " + std::string {feature.argumentPrefix} +
                                std::string {value} + " " +
                                std::string {bootDevices} + "\n");
            requireThrows<std::exception>([&] {
                (void)uml::android::readKernelCommandLine(path, prefixes);
            });
        }
    }
}

void theSkuPrefixIsTheOneTheGuestImportsFromTheOdmAxis()
{
    /* Android reads ro.boot.product.hardware.sku for the ODM manifest of the
     * SKU, and Build.ODM_SKU shows the value to applications. */
    require(features[0].name == "haptics");
    require(features[0].argumentPrefix ==
            "androidboot.product.hardware.sku=");
    require(features[0].argumentValue == "haptics");
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"every-bit-is-one-bit-of-its-own", everyBitIsOneBitOfItsOwn},
        {"a-collision-between-two-rows-is-detected",
         aCollisionBetweenTwoRowsIsDetected},
        {"the-haptics-bit-becomes-the-sku-argument",
         theHapticsBitBecomesTheSkuArgument},
        {"every-known-bit-makes-one-argument-in-table-order",
         everyKnownBitMakesOneArgumentInTableOrder},
        {"bits-the-runtime-has-no-row-for-are-reported-and-make-nothing",
         bitsTheRuntimeHasNoRowForAreReportedAndMakeNothing},
        {"a-release-command-line-cannot-set-a-feature-argument",
         aReleaseCommandLineCannotSetAFeatureArgument},
        {"the-sku-prefix-is-the-one-the-guest-imports-from-the-odm-axis",
         theSkuPrefixIsTheOneTheGuestImportsFromTheOdmAxis},
    };
    return uml::testing::runNamedTests(tests);
}
