#include "ast/treemap/treemap.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::capybara {

namespace {

// Capybara's feature specs (capybara/rspec/features.rb) are RSpec under other names:
// `feature` is `describe` with `type: :feature`, `scenario` is `it`, `background` is `before`, and `given`/`given!`
// are `let`/`let!`. Renaming them lets Sorbet's RSpec rewriter (--enable-experimental-rspec) type-check these files.
struct Aliases {
    core::NameRef feature, xfeature, ffeature, scenario, xscenario, fscenario, background, given, givenBang;

    explicit Aliases(const core::GlobalState &gs)
        : feature(gs.lookupNameUTF8("feature")), xfeature(gs.lookupNameUTF8("xfeature")),
          ffeature(gs.lookupNameUTF8("ffeature")), scenario(gs.lookupNameUTF8("scenario")),
          xscenario(gs.lookupNameUTF8("xscenario")), fscenario(gs.lookupNameUTF8("fscenario")),
          background(gs.lookupNameUTF8("background")), given(gs.lookupNameUTF8("given")),
          givenBang(gs.lookupNameUTF8("given!")) {}

    // The RSpec name for a Capybara alias, or no name. A name that isn't in the name table can't appear in this
    // file, so its lookup returns no name and never matches.
    core::NameRef rspecName(core::NameRef fun) const {
        if (!fun.exists()) {
            return core::NameRef::noName();
        }
        if (fun == feature) {
            return core::Names::describe();
        }
        if (fun == xfeature) {
            return core::Names::xdescribe();
        }
        if (fun == ffeature) {
            return core::Names::fdescribe();
        }
        if (fun == scenario) {
            return core::Names::it();
        }
        if (fun == xscenario) {
            return core::Names::xit();
        }
        if (fun == fscenario) {
            return core::Names::fit();
        }
        if (fun == background) {
            return core::Names::before();
        }
        if (fun == given) {
            return core::Names::let();
        }
        if (fun == givenBang) {
            return core::Names::let_bang();
        }
        return core::NameRef::noName();
    }
};

bool isRSpecConstant(const ast::ExpressionPtr &expr) {
    auto cnst = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    return cnst != nullptr && cnst->cnst == core::Names::Constants::RSpec() && ast::MK::isRootScope(cnst->scope);
}

// Renames every alias call with a block inside one top-level example group.
class Renamer {
    const Aliases &aliases;

public:
    explicit Renamer(const Aliases &aliases) : aliases(aliases) {}

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (!send.hasBlock() || !send.recv.isSelfReference()) {
            return;
        }
        auto renamed = aliases.rspecName(send.fun);
        if (renamed.exists()) {
            send.fun = renamed;
        }
    }
};

// Top-level example groups: `RSpec.feature`/`RSpec.describe` and friends, or bare `feature` at the top of a file.
bool isTopLevelGroup(const Aliases &aliases, const ast::Send &send) {
    if (!send.hasBlock()) {
        return false;
    }
    if (!isRSpecConstant(send.recv) && !send.recv.isSelfReference()) {
        return false;
    }
    auto fun = aliases.rspecName(send.fun);
    if (!fun.exists()) {
        fun = send.fun;
    }
    switch (fun.rawId()) {
        case core::Names::describe().rawId():
        case core::Names::xdescribe().rawId():
        case core::Names::fdescribe().rawId():
        case core::Names::context().rawId():
        case core::Names::sharedExamples().rawId():
        case core::Names::sharedContext().rawId():
        case core::Names::sharedExamplesFor().rawId():
            return true;
        default:
            return false;
    }
}

// `feature` adds `type: :feature` metadata (Capybara's alias_example_group_to). Keep it on the renamed `describe`,
// unless the call sets its own type.
ast::ExpressionPtr withFeatureType(core::MutableContext ctx, ast::Send &send) {
    auto typeName = ctx.state.enterNameUTF8("type");
    for (uint16_t i = 0; i < send.numKwArgs(); i++) {
        auto key = ast::cast_tree<ast::Literal>(send.getKwKey(i));
        if (key != nullptr && key->isSymbol() && key->asSymbol() == typeName) {
            return nullptr;
        }
    }
    auto loc = send.funLoc.copyWithZeroLength();
    ast::Send::ARGS_store args;
    for (auto &arg : send.posArgs()) {
        args.emplace_back(move(arg));
    }
    for (uint16_t i = 0; i < send.numKwArgs(); i++) {
        args.emplace_back(move(send.getKwKey(i)));
        args.emplace_back(move(send.getKwValue(i)));
    }
    args.emplace_back(ast::MK::Symbol(loc, typeName));
    args.emplace_back(ast::MK::Symbol(loc, ctx.state.enterNameUTF8("feature")));
    if (auto kwSplat = send.kwSplat()) {
        args.emplace_back(move(*kwSplat));
    }
    if (auto block = send.rawBlock()) {
        args.emplace_back(move(*block));
    }
    return ast::MK::Send(send.loc, move(send.recv), send.fun, send.funLoc, send.numPosArgs(), move(args), send.flags);
}

class CapybaraPlugin final : public Plugin {
public:
    string_view name() const override {
        return "capybara";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (classDef.symbol != core::Symbols::root()) {
            return;
        }
        Aliases aliases(ctx.state);
        if (!aliases.feature.exists() && !aliases.scenario.exists()) {
            return;
        }
        for (auto &stat : classDef.rhs) {
            auto send = ast::cast_tree<ast::Send>(stat);
            if (send == nullptr || !isTopLevelGroup(aliases, *send)) {
                continue;
            }
            Renamer renamer(aliases);
            ast::TreeWalk::apply(ctx, renamer, stat);
            // The walk renames inner calls; rename the group itself too (`RSpec.feature` has an RSpec receiver).
            send = ast::cast_tree<ast::Send>(stat);
            auto isFeature = send->fun == aliases.feature || send->fun == aliases.xfeature || send->fun == aliases.ffeature;
            auto renamed = aliases.rspecName(send->fun);
            if (renamed.exists()) {
                send->fun = renamed;
            }
            if (isFeature) {
                if (auto typed = withFeatureType(ctx, *send)) {
                    stat = move(typed);
                }
            }
        }
    }
};

SORBET_PLUGIN(CapybaraPlugin);

} // namespace

} // namespace sorbet::plugins::capybara
