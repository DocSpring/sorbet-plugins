#include "absl/algorithm/container.h"
#include "absl/strings/str_split.h"
#include "ast/treemap/treemap.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::factory_bot {

namespace {

// FactoryBot picks the factory, its traits and its attributes from Symbols, so Sorbet can't check a call like
// `create(:user, :admin, name: 'Ann')` or know that it returns a User. The factory_bot generator writes an RBI with a
// module per factory:
//
//   module FactoryBotTypes::User
//     Model = T.type_alias { ::User }
//     sig { params(traits: Symbol, name: T.untyped, email: T.untyped, ..., blk: ...).returns(Model) }
//     def self.create(*traits, name: T.unsafe(nil), email: T.unsafe(nil), ..., &blk); end
//     ...
//     module Traits
//       sig { returns(Symbol) }
//       def self.admin; end
//     end
//   end
//
// and this plugin rewrites calls in `typed: true` and stricter files to use it:
//
//   ::FactoryBotTypes::User.create(::FactoryBotTypes::User::Traits.admin, name: 'Ann')
//
// so an unknown factory, trait or attribute is an error, and the result is typed. It also gives `let`, `let!` and
// `subject` blocks that only build a factory a `sig`, so the let's method is typed too.
//
// The rewrite only changes what Sorbet checks; the code still calls FactoryBot at runtime.
enum class Shape { Single, List, Pair, Attributes };

struct Method {
    core::NameRef name;
    Shape shape;
};

class Names {
public:
    vector<Method> methods;
    core::NameRef factoryBot;

    explicit Names(const core::GlobalState &gs) : factoryBot(gs.lookupNameConstant("FactoryBot")) {
        add(gs, "create", Shape::Single);
        add(gs, "build", Shape::Single);
        add(gs, "build_stubbed", Shape::Single);
        add(gs, "create_list", Shape::List);
        add(gs, "build_list", Shape::List);
        add(gs, "build_stubbed_list", Shape::List);
        add(gs, "create_pair", Shape::Pair);
        add(gs, "build_pair", Shape::Pair);
        add(gs, "build_stubbed_pair", Shape::Pair);
        add(gs, "attributes_for", Shape::Attributes);
    }

    const Method *find(core::NameRef fun) const {
        auto found = absl::c_find_if(methods, [&](auto &method) { return method.name == fun; });
        return found == methods.end() ? nullptr : &*found;
    }

private:
    void add(const core::GlobalState &gs, string_view name, Shape shape) {
        auto ref = gs.lookupNameUTF8(name);
        if (ref.exists()) {
            methods.emplace_back(Method{ref, shape});
        }
    }
};

// `:submission_data_request` => `SubmissionDataRequest`. The generator uses the same rule.
string camelize(string_view snake) {
    string out;
    for (auto part : absl::StrSplit(snake, '_')) {
        if (part.empty()) {
            continue;
        }
        out += static_cast<char>(toupper(part[0]));
        out.append(part.substr(1));
    }
    return out;
}

ast::ExpressionPtr rootConstant(core::MutableContext ctx, core::LocOffsets loc, absl::Span<const string> parts) {
    auto scope = ast::MK::Constant(loc, core::Symbols::root());
    for (auto &part : parts) {
        scope = ast::MK::UnresolvedConstant(loc, move(scope), ctx.state.enterNameConstant(part));
    }
    return scope;
}

bool isFactoryBotConstant(const Names &names, const ast::ExpressionPtr &expr) {
    auto cnst = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    return cnst != nullptr && names.factoryBot.exists() && cnst->cnst == names.factoryBot &&
           ast::MK::isRootScope(cnst->scope);
}

// If `expr` is a call this plugin wrote (`::FactoryBotTypes::<Factory>.<method>(...)`), the factory's module name.
optional<string> rewrittenFactory(core::MutableContext ctx, const ast::ExpressionPtr &expr) {
    auto send = ast::cast_tree<ast::Send>(expr);
    if (send == nullptr) {
        return nullopt;
    }
    auto factory = ast::cast_tree<ast::UnresolvedConstantLit>(send->recv);
    if (factory == nullptr) {
        return nullopt;
    }
    auto types = ast::cast_tree<ast::UnresolvedConstantLit>(factory->scope);
    if (types == nullptr || types->cnst.shortName(ctx) != "FactoryBotTypes" || !ast::MK::isRootScope(types->scope)) {
        return nullopt;
    }
    return string(factory->cnst.shortName(ctx));
}

class Rewriter {
    const Names &names;
    int classDepth = 0;

public:
    explicit Rewriter(const Names &names) : names(names) {}

    // Nested classes were rewritten when Sorbet visited them.
    void preTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        classDepth++;
    }
    void postTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        classDepth--;
    }

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        if (classDepth > 0) {
            return;
        }
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        auto method = names.find(send.fun);
        if (method == nullptr || send.numPosArgs() == 0 ||
            !(send.recv.isSelfReference() || isFactoryBotConstant(names, send.recv))) {
            return;
        }
        auto factoryLit = ast::cast_tree<ast::Literal>(send.getPosArg(0));
        if (factoryLit == nullptr || !factoryLit->isSymbol()) {
            return;
        }

        auto factoryLoc = factoryLit->loc;
        auto factory = camelize(factoryLit->asSymbol().shortName(ctx));
        uint16_t firstTrait = method->shape == Shape::List ? 2 : 1;

        ast::Send::ARGS_store args;
        uint16_t numPosArgs = 0;
        for (uint16_t i = 1; i < send.numPosArgs(); i++) {
            auto &arg = send.getPosArg(i);
            auto trait = ast::cast_tree<ast::Literal>(arg);
            if (i >= firstTrait && trait != nullptr && trait->isSymbol()) {
                auto traits = rootConstant(ctx, trait->loc, {"FactoryBotTypes", factory, "Traits"});
                args.emplace_back(ast::MK::Send0(trait->loc, move(traits), trait->asSymbol(), trait->loc));
            } else {
                args.emplace_back(move(arg));
            }
            numPosArgs++;
        }
        for (uint16_t i = 0; i < send.numKwArgs(); i++) {
            args.emplace_back(move(send.getKwKey(i)));
            args.emplace_back(move(send.getKwValue(i)));
        }
        if (auto kwSplat = send.kwSplat()) {
            args.emplace_back(move(*kwSplat));
        }
        if (auto block = send.rawBlock()) {
            args.emplace_back(move(*block));
        }

        auto recv = rootConstant(ctx, factoryLoc, {"FactoryBotTypes", factory});
        tree = ast::MK::Send(send.loc, move(recv), send.fun, send.funLoc, numPosArgs, move(args), send.flags);
    }

    // `let(:user) { create(:user) }` gets `sig { returns(::FactoryBotTypes::User::Model) }` before it. This runs after
    // the calls in the block's body were rewritten.
    void postTransformBlock(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        if (classDepth > 0) {
            return;
        }
        auto &block = ast::cast_tree_nonnull<ast::Block>(tree);
        auto insSeq = ast::cast_tree<ast::InsSeq>(block.body);
        if (insSeq == nullptr) {
            return;
        }

        ast::InsSeq::STATS_store stats;
        for (auto &stat : insSeq->stats) {
            if (auto sig = sigForLet(ctx, stat)) {
                stats.emplace_back(move(sig));
            }
            stats.emplace_back(move(stat));
        }
        if (auto sig = sigForLet(ctx, insSeq->expr)) {
            stats.emplace_back(move(sig));
        }
        insSeq->stats = move(stats);
    }

private:
    ast::ExpressionPtr sigForLet(core::MutableContext ctx, const ast::ExpressionPtr &stat) {
        auto let = ast::cast_tree<ast::Send>(stat);
        if (let == nullptr || !let->recv.isSelfReference() || !let->hasBlock()) {
            return nullptr;
        }
        if (let->fun != core::Names::let() && let->fun != core::Names::let_bang() &&
            let->fun != core::Names::subject()) {
            return nullptr;
        }
        auto &body = let->block()->body;
        auto factory = rewrittenFactory(ctx, body);
        if (!factory.has_value()) {
            return nullptr;
        }
        auto method = names.find(ast::cast_tree_nonnull<ast::Send>(body).fun);
        if (method == nullptr || method->shape == Shape::Attributes) {
            return nullptr;
        }
        auto loc = let->loc.copyWithZeroLength();
        auto model = rootConstant(ctx, loc, {"FactoryBotTypes", *factory, "Model"});
        if (method->shape != Shape::Single) {
            model = ast::MK::Send1(loc, ast::MK::T_Array(loc), core::Names::squareBrackets(), loc, move(model));
        }
        return ast::MK::Sig0(loc, move(model));
    }
};

class FactoryBotPlugin final : public Plugin {
public:
    string_view name() const override {
        return "factory_bot";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (ctx.file.data(ctx).strictLevel < core::StrictLevel::True) {
            return;
        }
        Names names(ctx.state);
        if (names.methods.empty()) {
            return;
        }
        Rewriter rewriter(names);
        for (auto &stat : classDef.rhs) {
            ast::TreeWalk::apply(ctx, rewriter, stat);
        }
    }
};

SORBET_PLUGIN(FactoryBotPlugin);

} // namespace

} // namespace sorbet::plugins::factory_bot
