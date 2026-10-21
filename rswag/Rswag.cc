#include "absl/algorithm/container.h"
#include "ast/treemap/treemap.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::rswag {

namespace {

// rswag-specs' DSL (Rswag::Specs::ExampleGroupHelpers) builds RSpec example groups and examples:
//
//   path(template) { ... }          is describe(template) { ... }
//   get(summary) { ... }            is describe(:get) { ... }, and the same for post, put, patch, delete, head,
//                                   options and trace
//   response(code, description) { } is context(description) { ... }
//   run_test!(description) { |r| }  is a before hook that sends the request, plus it(description) { ... }
//
// This plugin renames them so Sorbet's RSpec rewriter (--enable-experimental-rspec) turns the groups into classes
// and the tests into methods. The verbs only count as groups directly inside a group: inside an example, `get` is a
// request spec's request.
enum class Scope { Group, Example };

struct Names {
    core::NameRef path, response, runTest;
    core::NameRef verbs[8];

    explicit Names(const core::GlobalState &gs)
        : path(gs.lookupNameUTF8("path")), response(gs.lookupNameUTF8("response")),
          runTest(gs.lookupNameUTF8("run_test!")),
          verbs{gs.lookupNameUTF8("get"),    gs.lookupNameUTF8("post"),    gs.lookupNameUTF8("put"),
                gs.lookupNameUTF8("patch"),  gs.lookupNameUTF8("delete"),  gs.lookupNameUTF8("head"),
                gs.lookupNameUTF8("options"), gs.lookupNameUTF8("trace")} {}

    bool isVerb(core::NameRef fun) const {
        return fun.exists() && absl::c_linear_search(verbs, fun);
    }
};

bool isRSpecConstant(const ast::ExpressionPtr &expr) {
    auto cnst = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    return cnst != nullptr && cnst->cnst == core::Names::Constants::RSpec() && ast::MK::isRootScope(cnst->scope);
}

bool isGroupName(core::NameRef fun) {
    switch (fun.rawId()) {
        case core::Names::describe().rawId():
        case core::Names::xdescribe().rawId():
        case core::Names::fdescribe().rawId():
        case core::Names::context().rawId():
        case core::Names::xcontext().rawId():
        case core::Names::fcontext().rawId():
        case core::Names::sharedExamples().rawId():
        case core::Names::sharedContext().rawId():
        case core::Names::sharedExamplesFor().rawId():
            return true;
        default:
            return false;
    }
}

// Sorbet's RSpec rewriter drops an example's block parameters. rswag passes the response to run_test!'s block (and
// apps that override run_test! often pass the parsed body), so keep each parameter as an untyped local instead.
void keepBlockParamsAsLocals(ast::Block &block) {
    if (block.params.empty()) {
        return;
    }
    ast::InsSeq::STATS_store assigns;
    for (auto &param : block.params) {
        auto loc = param.loc();
        assigns.emplace_back(
            ast::MK::Assign(loc, ast::MK::arg2Name(param), ast::MK::Unsafe(loc, ast::MK::Nil(loc))));
    }
    block.params.clear();

    auto loc = block.body.loc();
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(block.body)) {
        for (auto &stat : insSeq->stats) {
            assigns.emplace_back(move(stat));
        }
        insSeq->stats = move(assigns);
    } else {
        block.body = ast::MK::InsSeq(loc, move(assigns), move(block.body));
    }
}

bool isLoop(core::MutableContext ctx, const ast::Send &send) {
    auto name = send.fun.shortName(ctx);
    return name == "each" || name == "each_with_index" || name == "each_key" || name == "each_value" ||
           name == "each_pair";
}

class Renamer {
    const Names &names;
    vector<Scope> scopes;

public:
    explicit Renamer(const Names &names) : names(names), scopes{Scope::Group} {}

    void preTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (!send.hasBlock()) {
            return;
        }

        auto scope = Scope::Example;
        bool inGroup = scopes.back() == Scope::Group;
        bool selfRecv = send.recv.isSelfReference();
        if (inGroup && !selfRecv && isLoop(ctx, send)) {
            // `[...].each do |x| response(...) { ... } end` defines the same groups once per element.
            scope = Scope::Group;
        } else if (inGroup && (selfRecv || isRSpecConstant(send.recv))) {
            if (isGroupName(send.fun)) {
                scope = Scope::Group;
            } else if (selfRecv && send.fun == names.path && names.path.exists()) {
                send.fun = core::Names::describe();
                scope = Scope::Group;
            } else if (selfRecv && names.isVerb(send.fun)) {
                send.fun = core::Names::describe();
                scope = Scope::Group;
            } else if (selfRecv && send.fun == names.response && names.response.exists()) {
                send.fun = core::Names::context();
                scope = Scope::Group;
            } else if (selfRecv && send.fun == names.runTest && names.runTest.exists()) {
                send.fun = core::Names::it();
                keepBlockParamsAsLocals(*send.block());
            }
        }
        scopes.emplace_back(scope);
    }

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        if (ast::cast_tree_nonnull<ast::Send>(tree).hasBlock()) {
            scopes.pop_back();
        }
    }
};

class RswagPlugin final : public Plugin {
public:
    string_view name() const override {
        return "rswag";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (classDef.symbol != core::Symbols::root()) {
            return;
        }
        Names names(ctx.state);
        if (!names.runTest.exists() && !names.path.exists()) {
            return;
        }
        for (auto &stat : classDef.rhs) {
            auto send = ast::cast_tree<ast::Send>(stat);
            if (send == nullptr || !send->hasBlock() || !isGroupName(send->fun) ||
                !(isRSpecConstant(send->recv) || send->recv.isSelfReference())) {
                continue;
            }
            // The walk pushes a Group scope for this top-level group itself.
            Renamer renamer(names);
            ast::TreeWalk::apply(ctx, renamer, stat);
        }
    }
};

SORBET_PLUGIN(RswagPlugin);

} // namespace

} // namespace sorbet::plugins::rswag
