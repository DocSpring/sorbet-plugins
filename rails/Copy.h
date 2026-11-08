#ifndef SORBET_PLUGINS_RAILS_COPY_H
#define SORBET_PLUGINS_RAILS_COPY_H

// Copying what a Prism parse says into Sorbet's tree: sigs' types, a method's parameters, constant names. Every
// node gets the location the caller gives (a zero-length location in the file being rewritten), since the source
// may be another file.

#include "Source.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/core.h"

namespace sorbet::plugins::rails {

// A sig's parts: the type of each named parameter, and the return type (nullptr for `void`).
struct Sig {
    std::vector<std::pair<std::string, const pm_node_t *>> params;
    const pm_node_t *returns = nullptr;
};

// The parts of `sig { params(...).returns(...) }` (with `override`, `overridable` or `checked` anywhere in the
// chain), or nullopt for a sig it can't copy (generic methods, `bind`).
std::optional<Sig> readSig(const RubyFile &file, const pm_call_node_t *sig);

// A copy of a type expression (constants, calls like `T.nilable(X)` and `T::Array[X]`, hashes, arrays, symbols,
// strings, integers, nil, true, false), or nullopt for anything else.
std::optional<ast::ExpressionPtr> copyExpression(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file,
                                                 const pm_node_t *node);

struct Param {
    enum class Kind { Required, Optional, Rest, Keyword, OptionalKeyword, KeywordRest, Block };
    Kind kind;
    std::string name;
};

// A method's parameters, or nullopt for one the plugin doesn't copy (`...`, a destructured parameter, `**nil`).
// An anonymous `*`, `**` or `&` is named as Sorbet names it.
std::optional<std::vector<Param>> readParams(const RubyFile &file, const pm_parameters_node_t *params);
// A parameter for a synthesized method, with `T.unsafe(nil)` for any default, as Tapioca writes them.
ast::ExpressionPtr paramTree(core::MutableContext ctx, core::LocOffsets loc, const Param &param);

// `Foo::Bar` (or `::Foo::Bar`) as a tree.
ast::ExpressionPtr constantTree(core::MutableContext ctx, core::LocOffsets loc, const ConstantRef &ref);
// The name a class or module was written with in Sorbet's tree, if it's a constant.
std::optional<ConstantRef> constantRefOf(core::Context ctx, const ast::ExpressionPtr &expr);

// The sig args (name, type, name, type...) for `params` in the order of `params`, with the types from `sig`
// (`T.untyped` where it has none). Nullopt if a type can't be copied.
std::optional<ast::Send::ARGS_store> sigArgs(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file,
                                             const std::vector<Param> &params, const Sig *sig);

} // namespace sorbet::plugins::rails

#endif
