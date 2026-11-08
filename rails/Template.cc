#include "Template.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

struct Converter {
    core::MutableContext ctx;
    core::LocOffsets loc;
    const RubyFile &file;
    string_view label;
    optional<string> problem;

    void fail(const pm_node_t *node, string_view what) {
        if (!problem) {
            problem = fmt::format("bug in the plugin's {} template (its line {}): {}", label, file.line(node), what);
        }
    }

    ast::ExpressionPtr sigTree(const pm_call_node_t *sig) {
        auto block = reinterpret_cast<const pm_block_node_t *>(sig->block);
        auto stats = statements(block->body);
        if (stats.size() != 1) {
            fail(reinterpret_cast<const pm_node_t *>(sig), "a sig block with more than one statement");
            return ast::MK::EmptyTree();
        }
        auto body = copyExpression(ctx, loc, file, stats[0]);
        if (!body) {
            fail(stats[0], "a sig the plugin can't copy");
            return ast::MK::EmptyTree();
        }
        ast::Send::Flags flags;
        flags.isRewriterSynthesized = true;
        return ast::MK::Send0Block(loc, ast::MK::Constant(loc, core::Symbols::T_Sig_WithoutRuntime()),
                                   core::Names::sig(), loc, ast::MK::Block0(loc, move(*body)), flags);
    }

    // The statements of a module, class or `class << self` body. `singleton`: every `def` is a singleton method.
    void body(const vector<const pm_node_t *> &stats, ast::ClassDef::RHS_store &out, bool singleton) {
        bool isPrivate = false;
        // The sigs before the next `def`: more than one is an overload, as in Tapioca's RBIs.
        vector<const pm_call_node_t *> pendingSigs;
        for (auto stat : stats) {
            if (problem) {
                return;
            }
            switch (PM_NODE_TYPE(stat)) {
                case PM_MODULE_NODE: {
                    auto node = reinterpret_cast<const pm_module_node_t *>(stat);
                    auto name = constantRef(file, node->constant_path);
                    if (!name) {
                        fail(stat, "a module whose name isn't a constant");
                        return;
                    }
                    ast::ClassDef::RHS_store rhs;
                    body(statements(node->body), rhs, false);
                    out.emplace_back(ast::MK::Module(loc, loc, constantTree(ctx, loc, *name), move(rhs)));
                    break;
                }
                case PM_CLASS_NODE: {
                    auto node = reinterpret_cast<const pm_class_node_t *>(stat);
                    auto name = constantRef(file, node->constant_path);
                    if (!name) {
                        fail(stat, "a class whose name isn't a constant");
                        return;
                    }
                    ast::ClassDef::ANCESTORS_store ancestors;
                    if (node->superclass == nullptr) {
                        ancestors.emplace_back(ast::MK::Constant(loc, core::Symbols::todo()));
                    } else {
                        auto superclass = constantRef(file, node->superclass);
                        if (!superclass) {
                            fail(stat, "a superclass that isn't a constant");
                            return;
                        }
                        ancestors.emplace_back(constantTree(ctx, loc, *superclass));
                    }
                    ast::ClassDef::RHS_store rhs;
                    body(statements(node->body), rhs, false);
                    out.emplace_back(
                        ast::MK::Class(loc, loc, constantTree(ctx, loc, *name), move(ancestors), move(rhs)));
                    break;
                }
                case PM_SINGLETON_CLASS_NODE: {
                    auto node = reinterpret_cast<const pm_singleton_class_node_t *>(stat);
                    if (!PM_NODE_TYPE_P(node->expression, PM_SELF_NODE)) {
                        fail(stat, "a `class <<` that isn't `class << self`");
                        return;
                    }
                    body(statements(node->body), out, true);
                    break;
                }
                case PM_CALL_NODE: {
                    auto call = reinterpret_cast<const pm_call_node_t *>(stat);
                    auto name = callName(file, call);
                    auto args = arguments(call);
                    if (call->receiver != nullptr) {
                        fail(stat, "a call with a receiver");
                        return;
                    }
                    if (name == "sig" && call->block != nullptr && PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
                        pendingSigs.emplace_back(call);
                    } else if (name == "private" && args.empty() && call->block == nullptr) {
                        isPrivate = true;
                    } else if ((name == "include" || name == "extend") && args.size() == 1) {
                        auto mixin = constantRef(file, args[0]);
                        if (!mixin) {
                            fail(stat, "an include or extend of something that isn't a constant");
                            return;
                        }
                        out.emplace_back(ast::MK::Send1(loc, ast::MK::Self(loc), ctx.state.enterNameUTF8(name), loc,
                                                        constantTree(ctx, loc, *mixin)));
                    } else {
                        fail(stat, fmt::format("a call to `{}`", name));
                        return;
                    }
                    break;
                }
                case PM_DEF_NODE: {
                    auto def = reinterpret_cast<const pm_def_node_t *>(stat);
                    auto params = readParams(file, def->parameters);
                    if (!params) {
                        fail(stat, "a method with parameters the plugin doesn't copy");
                        return;
                    }
                    for (auto sig : pendingSigs) {
                        out.emplace_back(sigTree(sig));
                    }
                    pendingSigs.clear();
                    ast::MethodDef::PARAMS_store paramTrees;
                    for (auto &param : *params) {
                        paramTrees.emplace_back(paramTree(ctx, loc, param));
                    }
                    ast::MethodDef::Flags flags;
                    flags.isSelfMethod =
                        singleton || (def->receiver != nullptr && PM_NODE_TYPE_P(def->receiver, PM_SELF_NODE));
                    auto method =
                        ast::MK::SyntheticMethod(loc, loc, ctx.state.enterNameUTF8(file.name(def->name)),
                                                 move(paramTrees), ast::MK::RaiseTypedUnimplemented(loc), flags);
                    if (isPrivate) {
                        ast::Send::Flags privateFlags;
                        privateFlags.isPrivateOk = true;
                        out.emplace_back(ast::MK::Send1(loc, ast::MK::Self(loc), core::Names::private_(), loc,
                                                        move(method), privateFlags));
                    } else {
                        out.emplace_back(move(method));
                    }
                    break;
                }
                case PM_CONSTANT_WRITE_NODE: {
                    // `Elem = type_member { { fixed: ::Model } }`
                    auto write = reinterpret_cast<const pm_constant_write_node_t *>(stat);
                    auto value = write->value;
                    const pm_call_node_t *call =
                        PM_NODE_TYPE_P(value, PM_CALL_NODE) ? reinterpret_cast<const pm_call_node_t *>(value) : nullptr;
                    if (call == nullptr || call->receiver != nullptr || callName(file, call) != "type_member" ||
                        !arguments(call).empty() || call->block == nullptr ||
                        !PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
                        fail(stat, "a constant that isn't a `type_member` with a block");
                        return;
                    }
                    auto blockStats = statements(reinterpret_cast<const pm_block_node_t *>(call->block)->body);
                    optional<ast::ExpressionPtr> bounds;
                    if (blockStats.size() == 1) {
                        bounds = copyExpression(ctx, loc, file, blockStats[0]);
                    }
                    if (!bounds) {
                        fail(stat, "a `type_member` block the plugin can't copy");
                        return;
                    }
                    // On Sorbet::Private::Static (our build's payload defines it there), which Sorbet takes as a
                    // type member of the class, as it does `self.type_member`: a class without a superclass, such
                    // as `PrivateRelationWhereChain`, has no `type_member` of its own, since Tapioca doesn't extend
                    // it with T::Generic (an RBI doesn't need to).
                    auto name = ctx.state.enterNameConstant(file.name(write->name));
                    out.emplace_back(ast::MK::Assign(
                        loc, ast::MK::UnresolvedConstant(loc, ast::MK::EmptyTree(), name),
                        ast::MK::Send0Block(loc, ast::MK::Constant(loc, core::Symbols::Sorbet_Private_Static()),
                                            core::Names::typeMember(), loc, ast::MK::Block0(loc, move(*bounds)))));
                    break;
                }
                default:
                    fail(stat, "a statement that isn't a definition, a mixin, a `type_member`, `private` or a sig");
                    return;
            }
        }
        if (!pendingSigs.empty()) {
            fail(reinterpret_cast<const pm_node_t *>(pendingSigs.front()), "a sig without a method after it");
        }
    }
};

} // namespace

string replaceAll(string text, string_view from, string_view to) {
    for (size_t at = text.find(from); at != string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

void addTemplate(core::MutableContext ctx, core::LocOffsets loc, ast::ClassDef::RHS_store &rhs, string source,
                 string_view label) {
    auto file = make_shared<RubyFile>(fmt::format("<rails plugin {} template>", label), move(source));
    if (file->errorCount() > 0) {
        reportError(ctx, loc, fmt::format("bug in the plugin's {} template: Prism can't parse it", label));
        return;
    }
    Converter converter{ctx, loc, *file, label, nullopt};
    ast::ClassDef::RHS_store out;
    converter.body(file->topLevel(), out, false);
    if (converter.problem) {
        reportError(ctx, loc, *converter.problem);
        return;
    }
    for (auto &stat : out) {
        rhs.emplace_back(move(stat));
    }
}

} // namespace sorbet::plugins::rails
