#include "Copy.h"

#include <cstdlib>

using namespace std;

namespace sorbet::plugins::rails {

optional<Sig> readSig(const RubyFile &file, const pm_call_node_t *sig) {
    auto block = reinterpret_cast<const pm_block_node_t *>(sig->block);
    auto stats = statements(block->body);
    if (stats.size() != 1 || !PM_NODE_TYPE_P(stats[0], PM_CALL_NODE)) {
        return nullopt;
    }
    Sig out;
    bool returnsSeen = false;
    for (auto node = stats[0]; node != nullptr;) {
        if (!PM_NODE_TYPE_P(node, PM_CALL_NODE)) {
            return nullopt;
        }
        auto call = reinterpret_cast<const pm_call_node_t *>(node);
        auto name = callName(file, call);
        auto args = arguments(call);
        if (name == "params") {
            if (args.size() != 1 || !PM_NODE_TYPE_P(args[0], PM_KEYWORD_HASH_NODE)) {
                return nullopt;
            }
            auto hash = reinterpret_cast<const pm_keyword_hash_node_t *>(args[0]);
            for (size_t i = 0; i < hash->elements.size; i++) {
                auto element = hash->elements.nodes[i];
                if (!PM_NODE_TYPE_P(element, PM_ASSOC_NODE)) {
                    return nullopt;
                }
                auto assoc = reinterpret_cast<const pm_assoc_node_t *>(element);
                if (!PM_NODE_TYPE_P(assoc->key, PM_SYMBOL_NODE)) {
                    return nullopt;
                }
                out.params.emplace_back(stringValue(reinterpret_cast<const pm_symbol_node_t *>(assoc->key)->unescaped),
                                        assoc->value);
            }
        } else if (name == "returns") {
            if (args.size() != 1) {
                return nullopt;
            }
            out.returns = args[0];
            returnsSeen = true;
        } else if (name == "void") {
            returnsSeen = true;
        } else if (name != "override" && name != "overridable" && name != "checked" && name != "on_failure") {
            return nullopt;
        }
        node = call->receiver;
    }
    if (!returnsSeen) {
        return nullopt;
    }
    return out;
}

ast::ExpressionPtr constantTree(core::MutableContext ctx, core::LocOffsets loc, const ConstantRef &ref) {
    auto scope = ref.absolute ? ast::MK::Constant(loc, core::Symbols::root()) : ast::MK::EmptyTree();
    for (auto &part : ref.name) {
        scope = ast::MK::UnresolvedConstant(loc, move(scope), ctx.state.enterNameConstant(part));
    }
    return scope;
}

optional<ConstantRef> constantRefOf(core::Context ctx, const ast::ExpressionPtr &expr) {
    // Before isRootScope, which counts an empty scope as the root.
    if (ast::isa_tree<ast::EmptyTree>(expr)) {
        return ConstantRef{{}, false};
    }
    if (ast::MK::isRootScope(expr)) {
        return ConstantRef{{}, true};
    }
    auto lit = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    if (lit == nullptr) {
        return nullopt;
    }
    auto scope = constantRefOf(ctx, lit->scope);
    if (!scope) {
        return nullopt;
    }
    scope->name.emplace_back(lit->cnst.shortName(ctx));
    return scope;
}

optional<ast::ExpressionPtr> copyExpression(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file,
                                            const pm_node_t *node) {
    switch (PM_NODE_TYPE(node)) {
        case PM_CONSTANT_READ_NODE:
        case PM_CONSTANT_PATH_NODE: {
            auto ref = constantRef(file, node);
            if (!ref) {
                return nullopt;
            }
            return constantTree(ctx, loc, *ref);
        }
        case PM_PARENTHESES_NODE: {
            auto stats = statements(reinterpret_cast<const pm_parentheses_node_t *>(node)->body);
            if (stats.size() != 1) {
                return nullopt;
            }
            return copyExpression(ctx, loc, file, stats[0]);
        }
        case PM_NIL_NODE:
            return ast::MK::Nil(loc);
        case PM_TRUE_NODE:
            return ast::MK::True(loc);
        case PM_FALSE_NODE:
            return ast::MK::False(loc);
        case PM_SELF_NODE:
            return ast::MK::Self(loc);
        case PM_INTEGER_NODE: {
            auto text = file.slice(node->location);
            return ast::MK::Int(loc, strtoll(string(text).c_str(), nullptr, 10));
        }
        case PM_SYMBOL_NODE:
            return ast::MK::Symbol(
                loc, ctx.state.enterNameUTF8(stringValue(reinterpret_cast<const pm_symbol_node_t *>(node)->unescaped)));
        case PM_STRING_NODE:
            return ast::MK::String(
                loc, ctx.state.enterNameUTF8(stringValue(reinterpret_cast<const pm_string_node_t *>(node)->unescaped)));
        case PM_ARRAY_NODE: {
            auto array = reinterpret_cast<const pm_array_node_t *>(node);
            ast::Array::ENTRY_store entries;
            for (size_t i = 0; i < array->elements.size; i++) {
                auto entry = copyExpression(ctx, loc, file, array->elements.nodes[i]);
                if (!entry) {
                    return nullopt;
                }
                entries.emplace_back(move(*entry));
            }
            return ast::MK::Array(loc, move(entries));
        }
        case PM_HASH_NODE:
        case PM_KEYWORD_HASH_NODE: {
            const pm_node_list_t &elements = PM_NODE_TYPE_P(node, PM_HASH_NODE)
                                                 ? reinterpret_cast<const pm_hash_node_t *>(node)->elements
                                                 : reinterpret_cast<const pm_keyword_hash_node_t *>(node)->elements;
            ast::Hash::ENTRY_store keys;
            ast::Hash::ENTRY_store values;
            for (size_t i = 0; i < elements.size; i++) {
                if (!PM_NODE_TYPE_P(elements.nodes[i], PM_ASSOC_NODE)) {
                    return nullopt;
                }
                auto assoc = reinterpret_cast<const pm_assoc_node_t *>(elements.nodes[i]);
                auto key = copyExpression(ctx, loc, file, assoc->key);
                auto value = copyExpression(ctx, loc, file, assoc->value);
                if (!key || !value) {
                    return nullopt;
                }
                keys.emplace_back(move(*key));
                values.emplace_back(move(*value));
            }
            return ast::MK::Hash(loc, move(keys), move(values));
        }
        case PM_CALL_NODE: {
            auto call = reinterpret_cast<const pm_call_node_t *>(node);
            if (call->block != nullptr) {
                return nullopt;
            }
            ast::ExpressionPtr recv;
            if (call->receiver == nullptr) {
                recv = ast::MK::Self(loc);
            } else {
                auto copied = copyExpression(ctx, loc, file, call->receiver);
                if (!copied) {
                    return nullopt;
                }
                recv = move(*copied);
            }
            ast::Send::ARGS_store args;
            uint16_t numPosArgs = 0;
            ast::Send::ARGS_store kwargs;
            for (auto arg : arguments(call)) {
                if (PM_NODE_TYPE_P(arg, PM_KEYWORD_HASH_NODE)) {
                    auto hash = reinterpret_cast<const pm_keyword_hash_node_t *>(arg);
                    for (size_t i = 0; i < hash->elements.size; i++) {
                        if (!PM_NODE_TYPE_P(hash->elements.nodes[i], PM_ASSOC_NODE)) {
                            return nullopt;
                        }
                        auto assoc = reinterpret_cast<const pm_assoc_node_t *>(hash->elements.nodes[i]);
                        auto key = copyExpression(ctx, loc, file, assoc->key);
                        auto value = copyExpression(ctx, loc, file, assoc->value);
                        if (!key || !value) {
                            return nullopt;
                        }
                        kwargs.emplace_back(move(*key));
                        kwargs.emplace_back(move(*value));
                    }
                    continue;
                }
                auto copied = copyExpression(ctx, loc, file, arg);
                if (!copied) {
                    return nullopt;
                }
                args.emplace_back(move(*copied));
                numPosArgs++;
            }
            for (auto &kwarg : kwargs) {
                args.emplace_back(move(kwarg));
            }
            ast::Send::Flags flags;
            if (call->receiver == nullptr) {
                flags.isPrivateOk = true;
            }
            return ast::MK::Send(loc, move(recv), ctx.state.enterNameUTF8(callName(file, call)), loc, numPosArgs,
                                 move(args), flags);
        }
        default:
            return nullopt;
    }
}

optional<vector<Param>> readParams(const RubyFile &file, const pm_parameters_node_t *params) {
    vector<Param> out;
    if (params == nullptr) {
        return out;
    }
    auto named = [&](pm_constant_id_t id, Param::Kind kind) -> bool {
        if (id == 0) {
            return false;
        }
        out.emplace_back(Param{kind, string(file.name(id))});
        return true;
    };
    for (size_t i = 0; i < params->requireds.size; i++) {
        auto node = params->requireds.nodes[i];
        if (!PM_NODE_TYPE_P(node, PM_REQUIRED_PARAMETER_NODE) ||
            !named(reinterpret_cast<const pm_required_parameter_node_t *>(node)->name, Param::Kind::Required)) {
            return nullopt;
        }
    }
    for (size_t i = 0; i < params->optionals.size; i++) {
        if (!named(reinterpret_cast<const pm_optional_parameter_node_t *>(params->optionals.nodes[i])->name,
                   Param::Kind::Optional)) {
            return nullopt;
        }
    }
    if (params->rest != nullptr) {
        if (!PM_NODE_TYPE_P(params->rest, PM_REST_PARAMETER_NODE) ||
            !named(reinterpret_cast<const pm_rest_parameter_node_t *>(params->rest)->name, Param::Kind::Rest)) {
            return nullopt;
        }
    }
    for (size_t i = 0; i < params->posts.size; i++) {
        auto node = params->posts.nodes[i];
        if (!PM_NODE_TYPE_P(node, PM_REQUIRED_PARAMETER_NODE) ||
            !named(reinterpret_cast<const pm_required_parameter_node_t *>(node)->name, Param::Kind::Required)) {
            return nullopt;
        }
    }
    for (size_t i = 0; i < params->keywords.size; i++) {
        auto node = params->keywords.nodes[i];
        if (PM_NODE_TYPE_P(node, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
            named(reinterpret_cast<const pm_required_keyword_parameter_node_t *>(node)->name, Param::Kind::Keyword);
        } else {
            named(reinterpret_cast<const pm_optional_keyword_parameter_node_t *>(node)->name,
                  Param::Kind::OptionalKeyword);
        }
    }
    if (params->keyword_rest != nullptr) {
        if (!PM_NODE_TYPE_P(params->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE) ||
            !named(reinterpret_cast<const pm_keyword_rest_parameter_node_t *>(params->keyword_rest)->name,
                   Param::Kind::KeywordRest)) {
            return nullopt;
        }
    }
    if (params->block != nullptr && !named(params->block->name, Param::Kind::Block)) {
        return nullopt;
    }
    return out;
}

ast::ExpressionPtr paramTree(core::MutableContext ctx, core::LocOffsets loc, const Param &param) {
    auto name = ctx.state.enterNameUTF8(param.name);
    auto unsafeNil = [&]() { return ast::MK::Unsafe(loc, ast::MK::Nil(loc)); };
    switch (param.kind) {
        case Param::Kind::Required:
            return ast::MK::Local(loc, name);
        case Param::Kind::Optional:
            return ast::MK::OptionalParam(loc, ast::MK::Local(loc, name), unsafeNil());
        case Param::Kind::Rest:
            return ast::MK::RestParam(loc, ast::MK::Local(loc, name));
        case Param::Kind::Keyword:
            return ast::MK::KeywordArg(loc, name);
        case Param::Kind::OptionalKeyword:
            return ast::MK::OptionalParam(loc, ast::MK::KeywordArg(loc, name), unsafeNil());
        case Param::Kind::KeywordRest:
            return ast::MK::RestParam(loc, ast::MK::KeywordArg(loc, name));
        case Param::Kind::Block:
            return ast::MK::BlockParam(loc, ast::MK::Local(loc, name));
    }
}

optional<ast::Send::ARGS_store> sigArgs(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file,
                                        const vector<Param> &params, const Sig *sig) {
    ast::Send::ARGS_store args;
    for (auto &param : params) {
        args.emplace_back(ast::MK::Symbol(loc, ctx.state.enterNameUTF8(param.name)));
        const pm_node_t *type = nullptr;
        if (sig != nullptr) {
            for (auto &[name, node] : sig->params) {
                if (name == param.name) {
                    type = node;
                }
            }
        }
        if (type == nullptr) {
            args.emplace_back(ast::MK::Untyped(loc));
            continue;
        }
        auto copied = copyExpression(ctx, loc, file, type);
        if (!copied) {
            return nullopt;
        }
        args.emplace_back(move(*copied));
    }
    return args;
}

} // namespace sorbet::plugins::rails
