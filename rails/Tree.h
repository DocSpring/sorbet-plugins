#ifndef SORBET_PLUGINS_RAILS_TREE_H
#define SORBET_PLUGINS_RAILS_TREE_H

// Changing the tree of the file Sorbet is rewriting: finding a class in it by full name, adding methods, and adding
// modules that keep another file's constant scope.

#include "Copy.h"
#include "Source.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/core.h"
#include "plugins/Plugin.h"

namespace sorbet::plugins::rails {

struct TreeClass {
    ast::ClassDef *def;
    ConstantName fullName;
};

// Every class and module in the file's tree (not `class << self`), outermost first.
std::vector<TreeClass> treeClasses(core::Context ctx, ast::ClassDef &root);
// The first class or module in the tree with this full name.
ast::ClassDef *findTreeClass(core::Context ctx, ast::ClassDef &root, const ConstantName &fullName);

// `sig` + `def` for a synthesized method with an empty body that raises.
void addMethod(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc, std::string_view name,
               ast::MethodDef::PARAMS_store params, ast::Send::ARGS_store sigArgs,
               std::optional<ast::ExpressionPtr> returns, bool singleton);

// Appends `module <name>; <body>; end` to the root, inside `def`'s own lexical nesting reopened (each enclosing
// module or class written as in `def`'s file), so constants in the body resolve as they do in that file. Returns the
// module's full name.
ConstantName addUnderNesting(core::MutableContext ctx, ast::ClassDef &root, core::LocOffsets loc, const Definition &def,
                             std::string_view name, ast::ClassDef::RHS_store body);

// `extend ::Full::Name` (or include).
ast::ExpressionPtr mixinStatement(core::MutableContext ctx, core::LocOffsets loc, std::string_view method,
                                  const ConstantName &fullName);

// The rails plugin's error, with a message that says what to do.
void reportError(core::MutableContext ctx, core::LocOffsets loc, std::string_view message);

// A zero-length location at the start of a class's declaration, for synthesized code.
core::LocOffsets zeroAt(const ast::ClassDef &def);

} // namespace sorbet::plugins::rails

#endif
