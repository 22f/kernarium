/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace uml::cpu {

/* Whether a run enters ahead-of-time code (cpu/arm64/aot): `automatic` is on
 * in a build that has the component unless DARWIN_UML_AOT=0, `off` and `on`
 * are a launch's own word (--aot=), which the environment does not override. */
enum class AotSelection : unsigned {
	automatic,
	off,
	on,
};

} // namespace uml::cpu
