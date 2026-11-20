// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAotRelease.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using namespace kernarium::aot;
using uml::testing::require;

/* A request of the current layout that states `disabled`. */
kernarium_runtime_boot request(std::uint32_t disabled)
{
    kernarium_runtime_boot boot {};
    boot.struct_size = static_cast<std::uint32_t>(sizeof(boot));
    boot.android_aot_disabled = disabled;
    return boot;
}

void aRequestThatStatesNothingKeepsTheComponentIn()
{
    require(!aotDisabled(request(0U)));
    /* A zeroed request, as Kernarium makes one, is today's launch. */
    kernarium_runtime_boot zeroed {};
    zeroed.struct_size = static_cast<std::uint32_t>(sizeof(zeroed));
    require(!aotDisabled(zeroed));
}

void anyNonzeroWordKeepsTheComponentOut()
{
    require(aotDisabled(request(1U)));
    require(aotDisabled(request(2U)));
    require(aotDisabled(request(0xffffffffU)));
}

void aRequestThatEndsBeforeTheWordNeverStatesIt()
{
    /* An older application's request is shorter than the word, and the bytes
     * past its struct_size are not its own: whatever they hold is not read. */
    kernarium_runtime_boot boot = request(1U);
    boot.struct_size = static_cast<std::uint32_t>(bootSizeBeforeAotDisabled);
    require(!aotDisabled(boot));
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeAotDisabled + sizeof(boot.android_aot_disabled) - 1U);
    require(!aotDisabled(boot));
    boot.struct_size = static_cast<std::uint32_t>(
        bootSizeBeforeAotDisabled + sizeof(boot.android_aot_disabled));
    require(aotDisabled(boot));
}

void theWordIsTheLastMemberAndTheApiIsTheOneThatAddedIt()
{
    /* A later field is a later API: nothing may follow the word unnoticed,
     * because every request that ends before it would then state it too. */
    require(bootSizeBeforeAotDisabled + sizeof(std::uint32_t) <=
            sizeof(kernarium_runtime_boot));
    require(sizeof(kernarium_runtime_boot) - bootSizeBeforeAotDisabled <=
            sizeof(void*));
    require(KERNARIUM_RUNTIME_API_VERSION >= 44U);
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"a-request-that-states-nothing-keeps-the-component-in",
         aRequestThatStatesNothingKeepsTheComponentIn},
        {"any-nonzero-word-keeps-the-component-out",
         anyNonzeroWordKeepsTheComponentOut},
        {"a-request-that-ends-before-the-word-never-states-it",
         aRequestThatEndsBeforeTheWordNeverStatesIt},
        {"the-word-is-the-last-member-and-the-api-is-the-one-that-added-it",
         theWordIsTheLastMemberAndTheApiIsTheOneThatAddedIt},
    };
    return uml::testing::runNamedTests(tests);
}
