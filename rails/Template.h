#ifndef SORBET_PLUGINS_RAILS_TEMPLATE_H
#define SORBET_PLUGINS_RAILS_TEMPLATE_H

// Templates: methods written as RBI-shaped Ruby, parsed with Prism and added to Sorbet's tree. The model part writes
// hundreds of methods per model (Tapioca's relation, column and association methods), which read far better as the
// RBI they stand for than as tree-building calls.

#include "Tree.h"

namespace sorbet::plugins::rails {

// The files in templates/, built into the plugin (BUILD).
namespace templates {
extern const std::string_view relations;
}

// Adds what `source` defines to `rhs`. `source` may contain only `module` and `class` definitions (with a constant
// superclass), `class << self`, `include` and `extend` of constants, a bare `private`, `Elem = type_member { ... }`,
// and `def`s, each with the `sig`s before it (more than one is an overload), copied whole (`type_parameters`,
// `T.proc` and the rest). The methods get empty bodies. Every node gets `loc`. Anything else is a bug in the plugin:
// it reports an error naming `label` and the template's line, and adds nothing.
void addTemplate(core::MutableContext ctx, core::LocOffsets loc, ast::ClassDef::RHS_store &rhs, std::string source,
                 std::string_view label);

// `text` with every `from` replaced with `to`.
std::string replaceAll(std::string text, std::string_view from, std::string_view to);

} // namespace sorbet::plugins::rails

#endif
