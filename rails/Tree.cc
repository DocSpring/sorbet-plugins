#include "Tree.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

void collectTree(core::Context ctx, ast::ClassDef &def, const ConstantName &outer, vector<TreeClass> &out) {
    for (auto &stat : def.rhs) {
        auto inner = ast::cast_tree<ast::ClassDef>(stat);
        if (inner == nullptr) {
            continue;
        }
        auto ref = constantRefOf(ctx, inner->name);
        if (!ref || ref->name.empty()) {
            continue;
        }
        ConstantName fullName = ref->absolute ? ConstantName{} : outer;
        fullName.insert(fullName.end(), ref->name.begin(), ref->name.end());
        out.emplace_back(TreeClass{inner, fullName});
        collectTree(ctx, *inner, fullName, out);
    }
}

} // namespace

vector<TreeClass> treeClasses(core::Context ctx, ast::ClassDef &root) {
    vector<TreeClass> out;
    collectTree(ctx, root, {}, out);
    return out;
}

ast::ClassDef *findTreeClass(core::Context ctx, ast::ClassDef &root, const ConstantName &fullName) {
    for (auto &found : treeClasses(ctx, root)) {
        if (found.fullName == fullName) {
            return found.def;
        }
    }
    return nullptr;
}

void addMethod(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc, string_view name,
               ast::MethodDef::PARAMS_store params, ast::Send::ARGS_store sigArgs, optional<ast::ExpressionPtr> returns,
               bool singleton) {
    if (returns) {
        body.emplace_back(ast::MK::Sig(loc, move(sigArgs), move(*returns)));
    } else {
        body.emplace_back(ast::MK::SigVoid(loc, move(sigArgs)));
    }
    ast::MethodDef::Flags flags;
    flags.isSelfMethod = singleton;
    body.emplace_back(ast::MK::SyntheticMethod(loc, loc, ctx.state.enterNameUTF8(name), move(params),
                                               ast::MK::RaiseTypedUnimplemented(loc), flags));
}

namespace {

// Whether every type in a sig can be copied.
bool copyable(core::MutableContext ctx, core::LocOffsets loc, const RubyFile &file, const Sig &sig) {
    for (auto &[name, type] : sig.params) {
        if (!copyExpression(ctx, loc, file, type)) {
            return false;
        }
    }
    return sig.returns == nullptr || copyExpression(ctx, loc, file, sig.returns).has_value();
}

} // namespace

optional<CopiedMethod> readMethod(core::MutableContext ctx, core::LocOffsets loc, const Definition &owner,
                                  const Definition::Method &method, string_view label) {
    auto &file = *owner.file;
    auto params = readParams(file, method.def->parameters);
    if (!params) {
        reportError(ctx, loc,
                    fmt::format("can't copy the parameters of {} ({}:{}): give each one a name", label, file.path,
                                file.line(reinterpret_cast<const pm_node_t *>(method.def))));
        return nullopt;
    }
    optional<Sig> sig;
    if (method.sig != nullptr) {
        sig = readSig(file, method.sig);
        if (!sig || !copyable(ctx, loc, file, *sig)) {
            reportError(ctx, loc,
                        fmt::format("can't copy the sig of {} ({}:{}): it has a part the plugin doesn't copy "
                                    "(generics, `bind`, a type that isn't a constant or a call)",
                                    label, file.path, file.line(reinterpret_cast<const pm_node_t *>(method.sig))));
            return nullopt;
        }
    }
    return CopiedMethod{owner.file, move(*params), move(sig)};
}

void addCopiedMethod(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc, string_view name,
                     const CopiedMethod &method, bool singleton) {
    ast::MethodDef::PARAMS_store params;
    for (auto &param : method.params) {
        params.emplace_back(paramTree(ctx, loc, param));
    }
    const Sig *sig = method.sig ? &*method.sig : nullptr;
    auto args = *sigArgs(ctx, loc, *method.file, method.params, sig);
    optional<ast::ExpressionPtr> returns;
    if (sig == nullptr) {
        returns = ast::MK::Untyped(loc);
    } else if (sig->returns != nullptr) {
        returns = copyExpression(ctx, loc, *method.file, sig->returns);
    }
    addMethod(ctx, body, loc, name, move(params), move(args), move(returns), singleton);
}

ConstantName addUnderNesting(core::MutableContext ctx, ast::ClassDef &root, core::LocOffsets loc, const Definition &def,
                             string_view name, ast::ClassDef::RHS_store body) {
    auto wrap = [&](bool isClass, const ConstantRef &written, ast::ExpressionPtr inner) {
        ast::ClassDef::RHS_store rhs;
        rhs.emplace_back(move(inner));
        if (isClass) {
            ast::ClassDef::ANCESTORS_store ancestors;
            ancestors.emplace_back(ast::MK::Constant(loc, core::Symbols::todo()));
            return ast::MK::Class(loc, loc, constantTree(ctx, loc, written), move(ancestors), move(rhs));
        }
        return ast::MK::Module(loc, loc, constantTree(ctx, loc, written), move(rhs));
    };
    auto tree = ast::MK::Module(loc, loc, constantTree(ctx, loc, ConstantRef{{string(name)}, false}), move(body));
    tree = wrap(def.isClass, def.written, move(tree));
    for (auto it = def.enclosing.rbegin(); it != def.enclosing.rend(); ++it) {
        tree = wrap(it->first, it->second, move(tree));
    }
    root.rhs.emplace_back(move(tree));
    auto fullName = def.fullName;
    fullName.emplace_back(name);
    return fullName;
}

ast::ExpressionPtr mixinStatement(core::MutableContext ctx, core::LocOffsets loc, string_view method,
                                  const ConstantName &fullName) {
    return ast::MK::Send1(loc, ast::MK::Self(loc), ctx.state.enterNameUTF8(method), loc,
                          constantTree(ctx, loc, ConstantRef{fullName, true}));
}

void reportError(core::MutableContext ctx, core::LocOffsets loc, string_view message) {
    if (auto e = ctx.beginIndexerError(loc, PluginError)) {
        e.setHeader("rails plugin: {}", message);
    }
}

core::LocOffsets zeroAt(const ast::ClassDef &def) {
    return def.declLoc.copyWithZeroLength();
}

} // namespace sorbet::plugins::rails
