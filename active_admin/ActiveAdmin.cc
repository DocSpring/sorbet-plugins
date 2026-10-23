// Types ActiveAdmin registrations. ActiveAdmin runs each block of `ActiveAdmin.register Model do ... end` with a
// different self: the registration block in a ResourceDSL, view blocks (`index`, `show`, `form`, ...) in an Arbre
// view with the controller's helpers, and `controller do` and action blocks in the resource's controller. Sorbet sees
// them all as blocks at the top level, so every call in them is an error on `T.class_of(<root>)`.
//
// The plugin:
// - binds the registration block's self to ActiveAdmin::ResourceDSL (PageDSL for `register_page`);
// - moves `controller do` bodies, and the blocks of `member_action`, `collection_action`, `page_action` and
//   `batch_action` (as methods with an implied `sig { void }`), into a stand-in controller class,
//   ActiveAdminTypes::<Resources|Pages>::<Name>::Controller;
// - binds view blocks' self to ActiveAdminTypes::<Resources|Pages>::<Name>::View;
// - types the resource block params (`show do |user|`, `column(:x) { |user| }` in an index or attributes table).
//
// generate_rbi.rb writes those classes from the app's registrations.

#include <cctype>
#include <functional>

#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "ast/treemap/treemap.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::active_admin {

namespace {

// Must match `camelize` in generate_rbi.rb: split on anything but letters and digits, upcase each part's first letter.
string camelize(string_view name) {
    string out;
    bool startOfPart = true;
    for (char c : name) {
        if (!isalnum(static_cast<unsigned char>(c))) {
            startOfPart = true;
            continue;
        }
        out += startOfPart ? static_cast<char>(toupper(static_cast<unsigned char>(c))) : c;
        startOfPart = false;
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

// `Ahoy::Visit` => "Ahoy::Visit", if `expr` is a constant path.
optional<string> constantPath(core::MutableContext ctx, const ast::ExpressionPtr &expr) {
    auto cnst = ast::cast_tree<ast::UnresolvedConstantLit>(expr);
    if (cnst == nullptr) {
        return nullopt;
    }
    string name(cnst->cnst.shortName(ctx));
    if (ast::isa_tree<ast::EmptyTree>(cnst->scope) || ast::MK::isRootScope(cnst->scope)) {
        return name;
    }
    auto scope = constantPath(ctx, cnst->scope);
    if (!scope.has_value()) {
        return nullopt;
    }
    return *scope + "::" + name;
}

bool isActiveAdmin(core::MutableContext ctx, const ast::ExpressionPtr &expr) {
    auto path = constantPath(ctx, expr);
    return path.has_value() && *path == "ActiveAdmin";
}

optional<string> stringOrSymbol(core::MutableContext ctx, const ast::ExpressionPtr &expr) {
    auto lit = ast::cast_tree<ast::Literal>(expr);
    if (lit == nullptr) {
        return nullopt;
    }
    if (lit->isString()) {
        return string(lit->asString().shortName(ctx));
    }
    if (lit->isSymbol()) {
        return string(lit->asSymbol().shortName(ctx));
    }
    return nullopt;
}

struct Names {
    core::NameRef registerName, registerPage, as, controller, memberAction, collectionAction, pageAction,
        batchAction, attributesTable, row, column;
    vector<core::NameRef> viewBlocks;
    vector<core::NameRef> resourceViewBlocks; // view blocks whose first block param is the resource
    vector<core::NameRef> otherRecords;

    explicit Names(const core::GlobalState &gs) {
        auto name = [&](string_view s) { return gs.lookupNameUTF8(s); };
        registerName = name("register");
        registerPage = name("register_page");
        as = name("as");
        controller = name("controller");
        memberAction = name("member_action");
        collectionAction = name("collection_action");
        pageAction = name("page_action");
        batchAction = name("batch_action");
        attributesTable = name("attributes_table");
        row = name("row");
        column = name("column");
        for (auto view : {"index", "show", "form", "csv", "sidebar", "action_item", "breadcrumb", "content"}) {
            viewBlocks.emplace_back(name(view));
        }
        for (auto view : {"index", "show"}) {
            resourceViewBlocks.emplace_back(name(view));
        }
        for (auto component : {"table_for", "attributes_table_for", "paginated_collection", "index_table_for"}) {
            otherRecords.emplace_back(name(component));
        }
    }

    bool exists(core::NameRef name) const {
        return name.exists();
    }

    // Components whose rows are other records: `table_for user.templates do column ...`.
    bool isOtherRecords(core::NameRef fun) const {
        return exists(fun) && absl::c_find(otherRecords, fun) != otherRecords.end();
    }

    bool isAction(core::NameRef fun) const {
        return exists(fun) && (fun == memberAction || fun == collectionAction || fun == pageAction);
    }

    bool isViewBlock(core::NameRef fun) const {
        return exists(fun) && absl::c_find(viewBlocks, fun) != viewBlocks.end();
    }

    bool isResourceViewBlock(core::NameRef fun) const {
        return exists(fun) && absl::c_find(resourceViewBlocks, fun) != resourceViewBlocks.end();
    }
};

struct Registration {
    bool page;
    vector<string> typePath; // ActiveAdminTypes, Resources|Pages, <Name>
    ast::ExpressionPtr model;
};

optional<Registration> registrationFor(core::MutableContext ctx, const Names &names, ast::Send &send) {
    if (!send.hasBlock() || send.numPosArgs() < 1 || !isActiveAdmin(ctx, send.recv) ||
        !(send.fun == names.registerName || send.fun == names.registerPage) || !names.exists(send.fun)) {
        return nullopt;
    }
    auto &arg = send.getPosArg(0);
    if (send.fun == names.registerPage) {
        auto name = stringOrSymbol(ctx, arg);
        if (!name.has_value()) {
            return nullopt;
        }
        return Registration{true, {"ActiveAdminTypes", "Pages", camelize(*name)}, nullptr};
    }
    auto model = constantPath(ctx, arg);
    if (!model.has_value()) {
        return nullopt;
    }
    auto name = *model;
    for (uint16_t i = 0; i < send.numKwArgs(); i++) {
        auto key = ast::cast_tree<ast::Literal>(send.getKwKey(i));
        if (key != nullptr && key->isSymbol() && key->asSymbol() == names.as) {
            if (auto as = stringOrSymbol(ctx, send.getKwValue(i))) {
                name = *as;
            }
        }
    }
    return Registration{false, {"ActiveAdminTypes", "Resources", camelize(name)}, arg.deepCopy()};
}

vector<string> withSuffix(const vector<string> &path, string suffix) {
    auto out = path;
    out.emplace_back(move(suffix));
    return out;
}

void prependStatements(ast::Block &block, ast::InsSeq::STATS_store stats) {
    if (stats.empty()) {
        return;
    }
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(block.body)) {
        for (auto &stat : insSeq->stats) {
            stats.emplace_back(move(stat));
        }
        insSeq->stats = move(stats);
    } else {
        auto loc = block.body.loc();
        block.body = ast::MK::InsSeq(loc, move(stats), move(block.body));
    }
}

// `T.bind(self, Type)`, as the cast node Sorbet's TypeAssertion rewriter makes of it (that rewriter has already run on
// the class body's sends).
void bindSelf(core::MutableContext ctx, ast::Block &block, ast::ExpressionPtr type) {
    auto loc = block.loc.copyWithZeroLength();
    ast::InsSeq::STATS_store stats;
    stats.emplace_back(ast::make_expression<ast::Cast>(loc, core::Types::todo(), ast::MK::Self(loc),
                                                       core::Names::syntheticBind(), move(type)));
    prependStatements(block, move(stats));
}

// Replaces the block's params with locals: the first one of `type` (`T.let(T.unsafe(nil), Type)`) when given, the rest
// untyped. Sorbet can't type a block param from the call's arguments.
void typeParams(ast::Block &block, const ast::ExpressionPtr &type) {
    for (auto &param : block.params) {
        if (!ast::isa_tree<ast::UnresolvedIdent>(param)) {
            return; // defaults, splats and destructuring stay as they are
        }
    }
    ast::InsSeq::STATS_store assigns;
    bool first = true;
    for (auto &param : block.params) {
        auto loc = param.loc();
        auto value = ast::MK::Unsafe(loc, ast::MK::Nil(loc));
        if (first && type != nullptr) {
            value = ast::MK::Let(loc, move(value), type.deepCopy());
        }
        first = false;
        assigns.emplace_back(ast::MK::Assign(loc, ast::MK::arg2Name(param), move(value)));
    }
    block.params.clear();
    prependStatements(block, move(assigns));
}

// `column(:x) { |user| }` and `row(:x) { |user| }` get the resource where it's the record: in an index, a CSV, and
// the attributes table of a show page. Elsewhere (`table_for other_records`) the param stays untyped.
class ResourceParams {
    const Names &names;
    const ast::ExpressionPtr &model;
    vector<bool> resourceContext;

public:
    ResourceParams(const Names &names, const ast::ExpressionPtr &model, bool initial)
        : names(names), model(model), resourceContext{initial} {}

    void preTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (!send.hasBlock()) {
            return;
        }
        bool context = false;
        if (send.recv.isSelfReference()) {
            bool cell = (send.fun == names.column || send.fun == names.row) && names.exists(send.fun);
            if (cell && resourceContext.back() && send.block()->params.size() == 1) {
                typeParams(*send.block(), model);
            } else if (send.fun == names.attributesTable && names.exists(send.fun) && send.numPosArgs() == 0) {
                context = true;
            } else if (!cell && !names.isOtherRecords(send.fun)) {
                // `panel`, `tabs`, `div`, layout `columns`: the same records as outside.
                context = resourceContext.back();
            }
        }
        resourceContext.emplace_back(context);
    }

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        if (ast::cast_tree_nonnull<ast::Send>(tree).hasBlock()) {
            resourceContext.pop_back();
        }
    }
};

void forEachStatement(ast::ExpressionPtr &body, const std::function<void(ast::ExpressionPtr &)> &fn) {
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        for (auto &stat : insSeq->stats) {
            fn(stat);
        }
        fn(insSeq->expr);
    } else {
        fn(body);
    }
}

void appendStatements(ast::ExpressionPtr body, ast::ClassDef::RHS_store &out) {
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        for (auto &stat : insSeq->stats) {
            out.emplace_back(move(stat));
        }
        out.emplace_back(move(insSeq->expr));
    } else if (!ast::isa_tree<ast::EmptyTree>(body)) {
        out.emplace_back(move(body));
    }
}

// Blocks ActiveAdmin and InheritedResources run in the controller instance: `before_create do |user|`,
// `permit_params do`, `scope_to do`, `before_action do`.
bool isControllerCallback(core::MutableContext ctx, core::NameRef fun) {
    auto name = fun.shortName(ctx);
    return absl::StartsWith(name, "before_") || absl::StartsWith(name, "after_") ||
           absl::StartsWith(name, "around_") || name == "permit_params" || name == "scope_to";
}

void bindControllerCallbacks(core::MutableContext ctx, const Registration &reg, ast::ExpressionPtr &body) {
    forEachStatement(body, [&](ast::ExpressionPtr &stat) {
        auto send = ast::cast_tree<ast::Send>(stat);
        if (send == nullptr || !send->recv.isSelfReference() || !send->hasBlock() ||
            !isControllerCallback(ctx, send->fun)) {
            return;
        }
        auto &block = *send->block();
        if (!block.params.empty()) {
            if (reg.model != nullptr) {
                typeParams(block, reg.model);
            } else {
                typeParams(block, ast::ExpressionPtr());
            }
        }
        bindSelf(ctx, block, rootConstant(ctx, block.loc, withSuffix(reg.typePath, "Controller")));
    });
}

// Rewrites one registration; returns its stand-in controller class, or nullptr when no blocks moved into it.
ast::ExpressionPtr rewriteRegistration(core::MutableContext ctx, const Names &names, ast::Send &send,
                                       const Registration &reg) {
    auto &block = *send.block();
    ast::ClassDef::RHS_store controllerBody;

    auto viewType = [&](core::LocOffsets loc) { return rootConstant(ctx, loc, withSuffix(reg.typePath, "View")); };
    forEachStatement(block.body, [&](ast::ExpressionPtr &stat) {
        auto inner = ast::cast_tree<ast::Send>(stat);
        if (inner == nullptr || !inner->recv.isSelfReference()) {
            return;
        }
        // Procs in options (`menu if: proc { current_user... }`, `filter :x, collection: -> { ... }`) run in the view.
        for (auto &arg : inner->nonBlockArgs()) {
            auto proc = ast::cast_tree<ast::Send>(arg);
            if (proc != nullptr && proc->hasBlock() && (proc->fun == core::Names::proc() || proc->fun == core::Names::lambda())) {
                bindSelf(ctx, *proc->block(), viewType(proc->loc));
            }
        }
        if (!inner->hasBlock()) {
            return;
        }
        auto &innerBlock = *inner->block();
        if (inner->fun == names.controller && names.exists(inner->fun) && inner->numPosArgs() == 0) {
            // `controller do ... end` runs in the controller class's body.
            bindControllerCallbacks(ctx, reg, innerBlock.body);
            appendStatements(move(innerBlock.body), controllerBody);
            stat = ast::MK::EmptyTree();
        } else if ((names.isAction(inner->fun) || (inner->fun == names.batchAction && names.exists(inner->fun))) &&
                   inner->numPosArgs() >= 1) {
            auto name = stringOrSymbol(ctx, inner->getPosArg(0));
            if (!name.has_value()) {
                return;
            }
            // The action's block runs as a controller method. Batch actions get `|ids, inputs|`, untyped here.
            typeParams(innerBlock, ast::ExpressionPtr());
            auto methodName = inner->fun == names.batchAction ? "batch_action_" + *name : *name;
            auto loc = inner->loc;
            // Nothing uses the block's value (Rails ignores what an action returns), and a `sig` can't go on a block:
            // an implied `sig { void }`, as Sorbet's Minitest rewriter gives test methods, so actions pass at
            // `typed: strict`.
            controllerBody.emplace_back(ast::MK::SigVoid(loc, {}));
            controllerBody.emplace_back(
                ast::MK::Method0(loc, inner->funLoc, ctx.state.enterNameUTF8(methodName), move(innerBlock.body)));
            inner->setBlock(nullptr, ast::Send::BlockType::None);
        } else if (names.isViewBlock(inner->fun)) {
            bool resourceBlock = !reg.page && names.isResourceViewBlock(inner->fun);
            if (!innerBlock.params.empty()) {
                if (inner->fun == ctx.state.lookupNameUTF8("form")) {
                    auto loc = innerBlock.params.front().loc();
                    typeParams(innerBlock, rootConstant(ctx, loc, {"ActiveAdmin", "FormBuilder"}));
                } else if (resourceBlock) {
                    typeParams(innerBlock, reg.model);
                } else {
                    typeParams(innerBlock, ast::ExpressionPtr());
                }
            }
            if (!reg.page) {
                ResourceParams resourceParams(names, reg.model,
                                              resourceBlock || inner->fun == ctx.state.lookupNameUTF8("csv"));
                ast::TreeWalk::apply(ctx, resourceParams, innerBlock.body);
            }
            bindSelf(ctx, innerBlock, viewType(innerBlock.loc));
        }
    });

    bindControllerCallbacks(ctx, reg, block.body);
    bindSelf(ctx, block,
             rootConstant(ctx, block.loc, {"ActiveAdmin", reg.page ? "PageDSL" : "ResourceDSL"}));

    if (controllerBody.empty()) {
        return nullptr;
    }
    auto loc = send.loc;
    auto declLoc = send.funLoc;
    ast::ClassDef::ANCESTORS_store ancestors;
    ancestors.emplace_back(ast::MK::Constant(loc.copyWithZeroLength(), core::Symbols::todo())); // superclass: the RBI's
    return ast::MK::Class(loc, declLoc, rootConstant(ctx, loc.copyWithZeroLength(), withSuffix(reg.typePath, "Controller")),
                          move(ancestors), move(controllerBody));
}

void visitStatement(core::MutableContext ctx, const Names &names, ast::ExpressionPtr &stat,
                    ast::ClassDef::RHS_store &controllers) {
    if (auto ifExpr = ast::cast_tree<ast::If>(stat)) {
        // `unless ENTERPRISE; ActiveAdmin.register_page ...; end`
        forEachStatement(ifExpr->thenp, [&](auto &inner) { visitStatement(ctx, names, inner, controllers); });
        forEachStatement(ifExpr->elsep, [&](auto &inner) { visitStatement(ctx, names, inner, controllers); });
        return;
    }
    auto send = ast::cast_tree<ast::Send>(stat);
    if (send == nullptr) {
        return;
    }
    if (auto reg = registrationFor(ctx, names, *send)) {
        if (auto controller = rewriteRegistration(ctx, names, *send, *reg)) {
            controllers.emplace_back(move(controller));
        }
    }
}

class ActiveAdminPlugin final : public Plugin {
public:
    string_view name() const override {
        return "active_admin";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        Names names(ctx.state);
        if (!names.exists(names.registerName) && !names.exists(names.registerPage)) {
            return;
        }
        ast::ClassDef::RHS_store controllers;
        for (auto &stat : classDef.rhs) {
            visitStatement(ctx, names, stat, controllers);
        }
        for (auto &controller : controllers) {
            classDef.rhs.emplace_back(move(controller));
        }
    }
};

SORBET_PLUGIN(ActiveAdminPlugin);

} // namespace

} // namespace sorbet::plugins::active_admin
