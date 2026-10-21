#include "absl/algorithm/container.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "ast/treemap/treemap.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins::rspec {

namespace {

// Helps Sorbet's RSpec rewriter (--enable-experimental-rspec) with three things it can't do alone:
//
// 1. What `config.include`/`config.extend` add to a group. Each top-level group (and any group with explicit
//    `type:` metadata) gets
//
//      include ::RSpecConfig::Everywhere
//      extend ::RSpecConfig::Extend::Everywhere
//      include ::RSpecConfig::Type::Request         # from `type: :request`, or the directory, as rspec-rails infers it
//      extend ::RSpecConfig::Extend::Type::Request
//
//    The rspec generator writes those modules by asking RSpec what a group of each type looks like at runtime.
//
// 2. Examples built in a loop: `%w[a b].each do |status| it "..." do ... end end` becomes Sorbet's own
//    `test_each(%w[a b]) do |status| ... end`, which types `status`. Only when every statement in the loop is one
//    `test_each` accepts.
//
// 3. Example block parameters: Sorbet drops `|example|` from `it`/`before`/`after` blocks. Keep it as a local typed
//    RSpec::Core::Example.

// rspec-rails' infer_spec_type_from_file_location! (RSpec::Rails::DIRECTORY_MAPPINGS).
struct DirectoryType {
    string_view dir;
    string_view type;
};
constexpr DirectoryType DIRECTORY_TYPES[] = {
    {"spec/channels/", "channel"}, {"spec/controllers/", "controller"}, {"spec/generator/", "generator"},
    {"spec/helpers/", "helper"},   {"spec/jobs/", "job"},               {"spec/mailers/", "mailer"},
    {"spec/models/", "model"},     {"spec/requests/", "request"},       {"spec/integration/", "request"},
    {"spec/api/", "request"},      {"spec/routing/", "routing"},        {"spec/views/", "view"},
    {"spec/features/", "feature"}, {"spec/system/", "system"},          {"spec/mailboxes/", "mailbox"},
};

optional<string_view> typeFromPath(string_view path) {
    for (auto &mapping : DIRECTORY_TYPES) {
        auto at = path.find(mapping.dir);
        if (at != string_view::npos && (at == 0 || path[at - 1] == '/' || path[at - 1] == '.')) {
            return mapping.type;
        }
    }
    return nullopt;
}

// `:api_proxy` => `ApiProxy`. The generator uses the same rule.
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
        case core::Names::exampleGroup().rawId():
        case core::Names::sharedExamples().rawId():
        case core::Names::sharedContext().rawId():
        case core::Names::sharedExamplesFor().rawId():
            return true;
        default:
            return false;
    }
}

bool isExampleName(core::NameRef fun) {
    switch (fun.rawId()) {
        case core::Names::it().rawId():
        case core::Names::fit().rawId():
        case core::Names::xit().rawId():
        case core::Names::specify().rawId():
        case core::Names::example().rawId():
        case core::Names::before().rawId():
        case core::Names::after().rawId():
            return true;
        default:
            return false;
    }
}

ast::ExpressionPtr rootConstant(core::MutableContext ctx, core::LocOffsets loc, absl::Span<const string> parts) {
    auto scope = ast::MK::Constant(loc, core::Symbols::root());
    for (auto &part : parts) {
        scope = ast::MK::UnresolvedConstant(loc, move(scope), ctx.state.enterNameConstant(part));
    }
    return scope;
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

// The group's `type: :foo` metadata, if it has one.
optional<string> explicitType(core::MutableContext ctx, core::NameRef typeName, const ast::Send &send) {
    if (!typeName.exists()) {
        return nullopt;
    }
    for (uint16_t i = 0; i < send.numKwArgs(); i++) {
        auto key = ast::cast_tree<ast::Literal>(send.getKwKey(i));
        auto value = ast::cast_tree<ast::Literal>(send.getKwValue(i));
        if (key != nullptr && key->isSymbol() && key->asSymbol() == typeName && value != nullptr && value->isSymbol()) {
            return string(value->asSymbol().shortName(ctx));
        }
    }
    return nullopt;
}

// Shared examples are modules, so they skip the type's class-level modules: rswag's ExampleGroupHelpers, for one,
// requires its singleton to be an example group class.
void addConfigModules(core::MutableContext ctx, ast::Send &group, optional<string> type, bool everywhere,
                      bool shared = false) {
    auto loc = group.funLoc.copyWithZeroLength();
    ast::InsSeq::STATS_store stats;
    auto add = [&](core::NameRef how, vector<string> parts) {
        stats.emplace_back(ast::MK::Send1(loc, ast::MK::Self(loc), how, loc, rootConstant(ctx, loc, parts)));
    };
    if (everywhere) {
        add(core::Names::include(), {"RSpecConfig", "Everywhere"});
        add(core::Names::extend(), {"RSpecConfig", "Extend", "Everywhere"});
    }
    if (type.has_value()) {
        auto typeModule = camelize(*type);
        add(core::Names::include(), {"RSpecConfig", "Type", typeModule});
        if (!shared) {
            add(core::Names::extend(), {"RSpecConfig", "Extend", "Type", typeModule});
        }
    }
    prependStatements(*group.block(), move(stats));
}

// Whether `expr` defines examples, groups, hooks or lets (a block passed to a call on self).
bool definesExamples(const ast::ExpressionPtr &expr) {
    if (auto send = ast::cast_tree<ast::Send>(expr)) {
        return send->recv.isSelfReference() && send->hasBlock();
    }
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(expr)) {
        return absl::c_any_of(insSeq->stats, definesExamples) || definesExamples(insSeq->expr);
    }
    if (auto ifExpr = ast::cast_tree<ast::If>(expr)) {
        return definesExamples(ifExpr->thenp) || definesExamples(ifExpr->elsep);
    }
    return false;
}

void appendFlattened(ast::ExpressionPtr stat, ast::InsSeq::STATS_store &out) {
    auto ifExpr = ast::cast_tree<ast::If>(stat);
    if (ifExpr != nullptr && definesExamples(stat)) {
        appendFlattened(move(ifExpr->thenp), out);
        appendFlattened(move(ifExpr->elsep), out);
    } else if (auto insSeq = ast::cast_tree<ast::InsSeq>(stat); insSeq != nullptr && ifExpr == nullptr) {
        for (auto &inner : insSeq->stats) {
            appendFlattened(move(inner), out);
        }
        appendFlattened(move(insSeq->expr), out);
    } else if (!ast::isa_tree<ast::EmptyTree>(stat)) {
        out.emplace_back(move(stat));
    }
}

// `if has_factory; it ...; end` in a group body: RSpec defines the examples only when the condition holds, but
// they'd be checked the same either way, and Sorbet only turns examples into methods at the top of a group's body.
// Keep both branches' statements and drop the condition.
void flattenConditionals(ast::ExpressionPtr &body) {
    auto hasConditional = [](const ast::ExpressionPtr &stat) {
        return ast::isa_tree<ast::If>(stat) && definesExamples(stat);
    };
    auto insSeq = ast::cast_tree<ast::InsSeq>(body);
    if (insSeq == nullptr ? !hasConditional(body)
                          : !(absl::c_any_of(insSeq->stats, hasConditional) || hasConditional(insSeq->expr))) {
        return;
    }
    auto loc = body.loc();
    ast::InsSeq::STATS_store stats;
    appendFlattened(move(body), stats);
    if (stats.empty()) {
        body = ast::MK::EmptyTree();
        return;
    }
    auto last = move(stats.back());
    stats.pop_back();
    body = stats.empty() ? move(last) : ast::MK::InsSeq(loc, move(stats), move(last));
}

// The same send with only its first `keepPos` positional arguments and its block.
ast::ExpressionPtr withoutMetadata(ast::Send &send, uint16_t keepPos) {
    ast::Send::ARGS_store args;
    for (uint16_t i = 0; i < keepPos; i++) {
        args.emplace_back(move(send.getPosArg(i)));
    }
    if (auto block = send.rawBlock()) {
        args.emplace_back(move(*block));
    }
    return ast::MK::Send(send.loc, move(send.recv), send.fun, send.funLoc, keepPos, move(args), send.flags);
}

void stripMetadataInBody(core::MutableContext ctx, core::NameRef typeName, ast::ExpressionPtr &body);

// `test_each` only accepts examples and groups with a single description, and hooks without a scope. Metadata and
// `before(:each)`'s scope don't change any types, so drop them inside a loop the plugin converts. A group's `type:`
// metadata becomes its config modules first.
void stripMetadata(core::MutableContext ctx, core::NameRef typeName, ast::ExpressionPtr &stat) {
    auto send = ast::cast_tree<ast::Send>(stat);
    if (send == nullptr || !send->recv.isSelfReference() || !send->hasBlock()) {
        return;
    }
    auto hasMetadata = [&](uint16_t keepPos) {
        return send->numPosArgs() > keepPos || send->numKwArgs() > 0 || send->kwSplat() != nullptr;
    };
    switch (send->fun.rawId()) {
        case core::Names::it().rawId():
        case core::Names::specify().rawId():
        case core::Names::example().rawId():
            if (send->numPosArgs() == 0) {
                auto loc = send->funLoc.copyWithZeroLength();
                send->insertPosArg(0, ast::MK::String(loc, send->fun));
            }
            if (hasMetadata(1)) {
                stat = withoutMetadata(*send, 1);
            }
            return;
        case core::Names::before().rawId():
        case core::Names::after().rawId():
            if (hasMetadata(0)) {
                stat = withoutMetadata(*send, 0);
            }
            return;
        case core::Names::describe().rawId():
        case core::Names::context().rawId():
            if (auto type = explicitType(ctx, typeName, *send)) {
                addConfigModules(ctx, *send, type, /* everywhere */ false);
            }
            stripMetadataInBody(ctx, typeName, send->block()->body);
            if (send->numPosArgs() >= 1 && hasMetadata(1)) {
                stat = withoutMetadata(*send, 1);
            }
            return;
        default:
            return;
    }
}

void stripMetadataInBody(core::MutableContext ctx, core::NameRef typeName, ast::ExpressionPtr &body) {
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        for (auto &stat : insSeq->stats) {
            stripMetadata(ctx, typeName, stat);
        }
        stripMetadata(ctx, typeName, insSeq->expr);
    } else {
        stripMetadata(ctx, typeName, body);
    }
}

// Whether `test_each` accepts `stat` (see runUnderParameterized in rewriter/Minitest.cc).
bool allowedInTestEach(const ast::ExpressionPtr &stat);

bool allowedBodyInTestEach(const ast::ExpressionPtr &body, bool skipLeadingAssigns = false) {
    if (ast::isa_tree<ast::EmptyTree>(body)) {
        return true;
    }
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        auto stats = absl::MakeConstSpan(insSeq->stats);
        // `|a, (b, c)|` desugars to assignments from Sorbet's destructuring parameter, which test_each copies.
        while (skipLeadingAssigns && !stats.empty() && ast::isa_tree<ast::Assign>(stats.front())) {
            stats.remove_prefix(1);
        }
        return absl::c_all_of(stats, allowedInTestEach) && allowedInTestEach(insSeq->expr);
    }
    return allowedInTestEach(body);
}

bool allowedInTestEach(const ast::ExpressionPtr &stat) {
    auto send = ast::cast_tree<ast::Send>(stat);
    if (send == nullptr || !send->recv.isSelfReference() || !send->hasBlock() || !send->block()->params.empty() ||
        send->numKwArgs() > 0 || send->kwSplat() != nullptr) {
        return false;
    }
    switch (send->fun.rawId()) {
        case core::Names::it().rawId():
        case core::Names::specify().rawId():
        case core::Names::example().rawId():
            return send->numPosArgs() == 1;
        case core::Names::before().rawId():
        case core::Names::after().rawId():
            return send->numPosArgs() == 0;
        case core::Names::let().rawId():
        case core::Names::let_bang().rawId():
        case core::Names::subject().rawId():
            return send->numPosArgs() == 0 || (send->numPosArgs() == 1 && ast::isa_tree<ast::Literal>(send->getPosArg(0)));
        case core::Names::describe().rawId():
        case core::Names::context().rawId():
            return send->numPosArgs() == 1 && allowedBodyInTestEach(send->block()->body);
        default:
            return false;
    }
}

// The first statement in `body` that test_each rejects, for SORBET_RSPEC_PLUGIN_DEBUG.
const ast::ExpressionPtr *firstRejected(const ast::ExpressionPtr &body, bool skipLeadingAssigns) {
    vector<const ast::ExpressionPtr *> stats;
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        bool leading = skipLeadingAssigns;
        for (auto &stat : insSeq->stats) {
            if (leading && ast::isa_tree<ast::Assign>(stat)) {
                continue;
            }
            leading = false;
            stats.emplace_back(&stat);
        }
        stats.emplace_back(&insSeq->expr);
    } else if (!ast::isa_tree<ast::EmptyTree>(body)) {
        stats.emplace_back(&body);
    }
    for (auto stat : stats) {
        if (allowedInTestEach(*stat)) {
            continue;
        }
        auto send = ast::cast_tree<ast::Send>(*stat);
        if (send != nullptr && send->hasBlock() && send->numPosArgs() == 1 &&
            (send->fun == core::Names::describe() || send->fun == core::Names::context())) {
            if (auto inner = firstRejected(send->block()->body, false)) {
                return inner;
            }
        }
        return stat;
    }
    return nullptr;
}

void debugRejected(core::MutableContext ctx, const ast::ExpressionPtr &body, bool skipLeadingAssigns) {
    static const bool enabled = getenv("SORBET_RSPEC_PLUGIN_DEBUG") != nullptr;
    if (!enabled) {
        return;
    }
    if (auto stat = firstRejected(body, skipLeadingAssigns)) {
        auto loc = ctx.locAt(stat->loc());
        auto source = loc.source(ctx).value_or("");
        auto firstLine = source.substr(0, source.find('\n'));
        fmt::print(stderr, "rspec-plugin: not test_each: {}:{}: {}\n", ctx.file.data(ctx).path(),
                   loc.toDetails(ctx).first.line, firstLine);
    }
}

// What test_each can copy into each example's method (canMoveIntoMethodDef in rewriter/Minitest.cc).
bool movable(const ast::ExpressionPtr &expr) {
    if (ast::isa_tree<ast::Literal>(expr) || ast::isa_tree<ast::UnresolvedConstantLit>(expr) ||
        ast::isa_tree<ast::EmptyTree>(expr)) {
        return true;
    }
    if (auto array = ast::cast_tree<ast::Array>(expr)) {
        return absl::c_all_of(array->elems, movable);
    }
    if (auto hash = ast::cast_tree<ast::Hash>(expr)) {
        return absl::c_all_of(hash->keys, movable) && absl::c_all_of(hash->values, movable);
    }
    if (auto send = ast::cast_tree<ast::Send>(expr)) {
        return movable(send->recv) && absl::c_all_of(send->nonBlockArgs(), movable);
    }
    return false;
}

bool isDestructureParam(core::MutableContext ctx, const ast::ExpressionPtr &param) {
    auto ident = ast::cast_tree<ast::UnresolvedIdent>(param);
    return ident != nullptr && ident->name.kind() == core::NameKind::UNIQUE &&
           ident->name.dataUnique(ctx)->original == core::Names::destructureArg();
}

// `|a, (b, c)|` desugars to `InsSeq{stats: [<b, c = <destructureArg>...>], expr: <the block's body>}` (see
// isDestructuringInsSeq in rewriter/Minitest.cc). The loop's statements are in `expr`.
ast::ExpressionPtr &loopBody(core::MutableContext ctx, ast::ExpressionPtr &body) {
    auto insSeq = ast::cast_tree<ast::InsSeq>(body);
    if (insSeq == nullptr || insSeq->stats.empty()) {
        return body;
    }
    bool destructuring = absl::c_all_of(insSeq->stats, [&](auto &stat) {
        auto inner = ast::cast_tree<ast::InsSeq>(stat);
        auto assign = inner == nullptr ? nullptr : ast::cast_tree<ast::Assign>(inner->stats.front());
        return assign != nullptr && isDestructureParam(ctx, assign->rhs);
    });
    return destructuring ? insSeq->expr : body;
}

// Loop bodies often compute a local for their examples (`label = x ? 'a' : 'b'`) or call a class-level macro
// (`parameter ...`). test_each accepts neither. Copy each local's assignment into every example, hook and let that
// follows it, so the examples still see its type, and drop the macros: they can't run inside an example, and keeping
// the loop out of test_each would leave every example in it unchecked.
void prependToExamples(ast::ExpressionPtr &stat, const vector<ast::ExpressionPtr> &assigns);

void prependToExamplesInBody(ast::ExpressionPtr &body, const vector<ast::ExpressionPtr> &assigns) {
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        for (auto &stat : insSeq->stats) {
            prependToExamples(stat, assigns);
        }
        prependToExamples(insSeq->expr, assigns);
    } else {
        prependToExamples(body, assigns);
    }
}

void moveLocalsAndDropMacros(core::MutableContext ctx, ast::ExpressionPtr &body, bool destructures);

// A class-level call without a block: rswag's `parameter`, `it_behaves_like` (its shared examples are checked on
// their own).
bool isMacro(const ast::ExpressionPtr &stat) {
    auto send = ast::cast_tree<ast::Send>(stat);
    return send != nullptr && send->recv.isSelfReference() && !send->hasBlock();
}

bool isNestedGroup(const ast::Send &send) {
    return (send.recv.isSelfReference() || isRSpecConstant(send.recv)) && send.hasBlock() &&
           (send.fun == core::Names::describe() || send.fun == core::Names::context());
}

bool definesGroup(const ast::ExpressionPtr &body) {
    auto isGroup = [](const ast::ExpressionPtr &stat) {
        auto send = ast::cast_tree<ast::Send>(stat);
        return send != nullptr && isNestedGroup(*send);
    };
    if (auto insSeq = ast::cast_tree<ast::InsSeq>(body)) {
        return absl::c_any_of(insSeq->stats, isGroup) || isGroup(insSeq->expr);
    }
    return isGroup(body);
}

void prependToExamples(ast::ExpressionPtr &stat, const vector<ast::ExpressionPtr> &assigns) {
    auto send = ast::cast_tree<ast::Send>(stat);
    if (send == nullptr || !send->hasBlock() || assigns.empty()) {
        return;
    }
    if (isNestedGroup(*send)) {
        prependToExamplesInBody(send->block()->body, assigns);
        return;
    }
    ast::InsSeq::STATS_store copies;
    for (auto &assign : assigns) {
        copies.emplace_back(assign.deepCopy());
    }
    prependStatements(*send->block(), move(copies));
}

bool loopToGroup(core::MutableContext ctx, ast::ExpressionPtr &stat, bool rootLevel);

// `next if skip` in a loop body only skips some elements' examples, which doesn't change what they'd be checked as.
bool isNextGuard(const ast::ExpressionPtr &stat) {
    auto ifExpr = ast::cast_tree<ast::If>(stat);
    return ifExpr != nullptr && ((ast::isa_tree<ast::Next>(ifExpr->thenp) && ast::isa_tree<ast::EmptyTree>(ifExpr->elsep)) ||
                                 (ast::isa_tree<ast::Next>(ifExpr->elsep) && ast::isa_tree<ast::EmptyTree>(ifExpr->thenp)));
}

void moveLocalsAndDropMacros(core::MutableContext ctx, ast::ExpressionPtr &body, bool destructures) {
    // Nested groups first: their own locals and macros (rswag's `parameter`, `it_behaves_like`) are handled the same
    // way. test_each can't nest, so a loop inside this one becomes a group.
    auto handleNested = [&](ast::ExpressionPtr &stat) {
        if (loopToGroup(ctx, stat, /* rootLevel */ false)) {
            return;
        }
        auto send = ast::cast_tree<ast::Send>(stat);
        if (send != nullptr && isNestedGroup(*send)) {
            moveLocalsAndDropMacros(ctx, send->block()->body, false);
        }
    };
    flattenConditionals(body);
    auto insSeq = ast::cast_tree<ast::InsSeq>(body);
    if (insSeq == nullptr) {
        if (isMacro(body)) {
            body = ast::MK::EmptyTree();
        }
        handleNested(body);
        return;
    }
    for (auto &stat : insSeq->stats) {
        handleNested(stat);
    }
    handleNested(insSeq->expr);
    vector<ast::ExpressionPtr> assigns;
    ast::InsSeq::STATS_store kept;
    bool leading = true;
    for (auto &stat : insSeq->stats) {
        auto assign = ast::cast_tree<ast::Assign>(stat);
        if (assign != nullptr && destructures && leading) {
            kept.emplace_back(move(stat)); // test_each copies destructuring assignments itself
            continue;
        }
        leading = false;
        if (assign != nullptr && ast::isa_tree<ast::UnresolvedIdent>(assign->lhs)) {
            assigns.emplace_back(stat.deepCopy());
            continue;
        }
        if (isMacro(stat) || isNextGuard(stat)) {
            continue;
        }
        prependToExamples(stat, assigns);
        kept.emplace_back(move(stat));
    }
    if (isMacro(insSeq->expr)) {
        insSeq->expr = ast::MK::EmptyTree();
    }
    prependToExamples(insSeq->expr, assigns);
    insSeq->stats = move(kept);
    if (insSeq->stats.empty()) {
        body = move(insSeq->expr);
    } else if (ast::isa_tree<ast::EmptyTree>(insSeq->expr)) {
        // test_each rejects an empty statement.
        insSeq->expr = move(insSeq->stats.back());
        insSeq->stats.pop_back();
        if (insSeq->stats.empty()) {
            body = move(insSeq->expr);
        }
    }
}

// Gets a block that becomes a test_each ready for it, and returns its statements.
ast::ExpressionPtr &prepareTestEachBody(core::MutableContext ctx, core::NameRef typeName, ast::Block &block) {
    bool destructures = absl::c_any_of(block.params, [&](auto &param) { return isDestructureParam(ctx, param); });
    auto &body = loopBody(ctx, block.body);
    bool leadingAssigns = destructures && &body == &block.body;
    stripMetadataInBody(ctx, typeName, body);
    moveLocalsAndDropMacros(ctx, body, leadingAssigns);
    return body;
}

bool isLoop(core::MutableContext ctx, const ast::Send &send) {
    if (!send.hasBlock() || send.numPosArgs() != 0 || send.numKwArgs() != 0 || send.recv.isSelfReference() ||
        send.block()->params.empty()) {
        return false;
    }
    auto iterator = send.fun.shortName(ctx);
    return send.fun == core::Names::each() || iterator == "each_key" || iterator == "each_value" ||
           iterator == "each_pair" || iterator == "each_with_index";
}

enum class LoopRewrite { None, TestEach, Empty };

// A loop test_each can't take (one inside another, or one around `RSpec.describe` at the top of a file) becomes a
// `describe` whose examples, hooks and lets start by setting the loop's variables to untyped values.
bool loopToGroup(core::MutableContext ctx, ast::ExpressionPtr &stat, bool rootLevel) {
    auto send = ast::cast_tree<ast::Send>(stat);
    if (send == nullptr || !isLoop(ctx, *send)) {
        return false;
    }
    auto &block = *send->block();
    for (auto &param : block.params) {
        if (!ast::isa_tree<ast::UnresolvedIdent>(param)) {
            return false;
        }
    }
    vector<ast::ExpressionPtr> assigns;
    for (auto &param : block.params) {
        auto loc = param.loc();
        assigns.emplace_back(ast::MK::Assign(loc, ast::MK::arg2Name(param), ast::MK::Unsafe(loc, ast::MK::Nil(loc))));
    }
    auto &body = loopBody(ctx, block.body);
    if (rootLevel && !definesGroup(body)) {
        return false;
    }
    if (&body != &block.body) {
        for (auto &destructure : ast::cast_tree_nonnull<ast::InsSeq>(block.body).stats) {
            assigns.emplace_back(destructure.deepCopy());
        }
    }
    ast::ExpressionPtr groupBody = move(body);
    ast::InsSeq::STATS_store classLevel;
    if (rootLevel) {
        // Descriptions and class-level conditions (`if has_factory`) use the loop's variables and locals too. A root
        // group isn't inside a test_each, so it can set them.
        for (auto &assign : assigns) {
            classLevel.emplace_back(assign.deepCopy());
        }
        auto insSeq = ast::cast_tree<ast::InsSeq>(groupBody);
        if (insSeq != nullptr) {
            for (auto &stat : insSeq->stats) {
                auto assign = ast::cast_tree<ast::Assign>(stat);
                if (assign != nullptr && ast::isa_tree<ast::UnresolvedIdent>(assign->lhs)) {
                    classLevel.emplace_back(stat.deepCopy());
                }
            }
        }
    }
    moveLocalsAndDropMacros(ctx, groupBody, false);
    prependToExamplesInBody(groupBody, assigns);

    auto loc = send->loc;
    auto zero = loc.copyWithZeroLength();
    block.params.clear();
    block.body = move(groupBody);
    prependStatements(block, move(classLevel));
    send->recv = rootLevel ? ast::MK::Constant(zero, core::Symbols::root()) : ast::MK::Self(zero);
    if (rootLevel) {
        send->recv = ast::MK::UnresolvedConstant(zero, move(send->recv), core::Names::Constants::RSpec());
    }
    send->fun = core::Names::describe();
    send->insertPosArg(0, ast::MK::String(zero, send->fun));
    return true;
}

// `%w[a b].each do |x| ... end` in a group body => `test_each(%w[a b]) do |x| ... end`. A loop of nothing but
// macros is emptied instead.
LoopRewrite rewriteEachToTestEach(core::MutableContext ctx, core::NameRef typeName, ast::Send &send) {
    if (!isLoop(ctx, send)) {
        return LoopRewrite::None;
    }
    bool isEach = send.fun == core::Names::each();
    auto &block = *send.block();
    bool destructures = false;
    for (auto &param : block.params) {
        if (!ast::isa_tree<ast::UnresolvedIdent>(param)) {
            return LoopRewrite::None; // defaults and splats stay as they are
        }
        destructures = destructures || isDestructureParam(ctx, param);
    }
    auto &body = prepareTestEachBody(ctx, typeName, block);
    bool leadingAssigns = destructures && &body == &block.body;
    if (ast::isa_tree<ast::EmptyTree>(body)) {
        // Left as an `each` with an empty block, which is fine at class level.
        body = ast::MK::EmptyTree();
        block.body = ast::MK::EmptyTree();
        return LoopRewrite::Empty;
    }
    if (!allowedBodyInTestEach(body, leadingAssigns)) {
        debugRejected(ctx, body, leadingAssigns);
        return LoopRewrite::None;
    }

    auto loc = send.recv.loc();
    bool isHash = isEach && ast::isa_tree<ast::Hash>(send.recv) && block.params.size() == 2;
    if (!isEach || !movable(send.recv)) {
        // test_each would replace it with a raise, which skips checking the examples. An untyped stand-in keeps them
        // checked, with an untyped loop variable.
        send.recv = ast::MK::Unsafe(loc, ast::MK::Nil(loc));
    }
    send.insertPosArg(0, move(send.recv));
    send.recv = ast::MK::Self(loc.copyWithZeroLength());
    send.fun = isHash ? core::Names::testEachHash() : core::Names::testEach();
    return LoopRewrite::TestEach;
}

void keepExampleParam(core::MutableContext ctx, ast::Block &block, vector<string> typeParts) {
    for (auto &param : block.params) {
        if (!ast::isa_tree<ast::UnresolvedIdent>(param)) {
            return;
        }
    }
    ast::InsSeq::STATS_store assigns;
    for (auto &param : block.params) {
        auto loc = param.loc();
        auto type = rootConstant(ctx, loc, typeParts);
        auto example = ast::MK::Let(loc, ast::MK::Unsafe(loc, ast::MK::Nil(loc)), move(type));
        assigns.emplace_back(ast::MK::Assign(loc, ast::MK::arg2Name(param), move(example)));
    }
    block.params.clear();
    prependStatements(block, move(assigns));
}

enum class Scope { Group, Other };

bool isSharedGroupName(core::NameRef fun) {
    return fun == core::Names::sharedExamples() || fun == core::Names::sharedContext() ||
           fun == core::Names::sharedExamplesFor();
}

class Walker {
    core::NameRef typeName;
    core::NameRef aroundName;
    vector<Scope> scopes;
    // The spec type in effect in each scope: the nearest `type:`, else the one rspec-rails infers from the path.
    vector<optional<string>> types;

public:
    Walker(core::NameRef typeName, core::NameRef aroundName, optional<string> pathType)
        : typeName(typeName), aroundName(aroundName), scopes{Scope::Group}, types{move(pathType)} {}

    void preTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (!send.hasBlock()) {
            return;
        }
        auto scope = Scope::Other;
        auto type = types.back();
        if (scopes.back() == Scope::Group) {
            if (isGroupName(send.fun) && (send.recv.isSelfReference() || isRSpecConstant(send.recv))) {
                auto explicitGroupType = explicitType(ctx, typeName, send);
                if (explicitGroupType.has_value()) {
                    type = explicitGroupType;
                }
                flattenConditionals(send.block()->body);
                if (isSharedGroupName(send.fun) && !send.block()->params.empty()) {
                    // Sorbet checks shared examples with parameters as a test_each over one element.
                    prepareTestEachBody(ctx, typeName, *send.block());
                }
                if (scopes.size() == 1 || isRSpecConstant(send.recv) || isSharedGroupName(send.fun)) {
                    // Top-level groups subclass ExampleGroup, and Sorbet makes shared examples modules: neither
                    // inherits an enclosing group's modules.
                    addConfigModules(ctx, send, type, /* everywhere */ true, isSharedGroupName(send.fun));
                } else if (explicitGroupType.has_value()) {
                    addConfigModules(ctx, send, explicitGroupType, /* everywhere */ false);
                }
                scope = Scope::Group;
            } else if (auto loop = rewriteEachToTestEach(ctx, typeName, send); loop != LoopRewrite::None) {
                scope = loop == LoopRewrite::TestEach ? Scope::Group : Scope::Other;
            } else if (isExampleName(send.fun) && send.recv.isSelfReference() && !send.block()->params.empty()) {
                keepExampleParam(ctx, *send.block(), {"RSpec", "Core", "Example"});
            } else if (send.recv.isSelfReference() && send.fun == aroundName && aroundName.exists()) {
                // `around { |example| ...; example.run }` runs in the example. Check it as a before hook.
                keepExampleParam(ctx, *send.block(), {"RSpec", "Core", "Example", "Procsy"});
                send.fun = core::Names::before();
                if (send.numPosArgs() > 0) {
                    tree = withoutMetadata(send, 0);
                }
            }
        }
        scopes.emplace_back(scope);
        types.emplace_back(move(type));
    }

    void postTransformSend(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        if (ast::cast_tree_nonnull<ast::Send>(tree).hasBlock()) {
            scopes.pop_back();
            types.pop_back();
        }
    }

    // Loops in a helper method run when it's called, not when the group is defined.
    void preTransformMethodDef(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        scopes.emplace_back(Scope::Other);
        types.emplace_back(types.back());
    }

    void postTransformMethodDef(core::MutableContext ctx, ast::ExpressionPtr &tree) {
        scopes.pop_back();
        types.pop_back();
    }
};

class RspecPlugin final : public Plugin {
public:
    string_view name() const override {
        return "rspec";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (classDef.symbol != core::Symbols::root()) {
            return;
        }
        auto typeName = ctx.state.lookupNameUTF8("type");
        auto aroundName = ctx.state.lookupNameUTF8("around");
        optional<string> pathType;
        if (auto inferred = typeFromPath(ctx.file.data(ctx).path())) {
            pathType = string(*inferred);
        }
        for (auto &stat : classDef.rhs) {
            loopToGroup(ctx, stat, /* rootLevel */ true);
            auto send = ast::cast_tree<ast::Send>(stat);
            if (send == nullptr || !send->hasBlock() || !isGroupName(send->fun) ||
                !(isRSpecConstant(send->recv) || send->recv.isSelfReference())) {
                continue;
            }
            Walker walker(typeName, aroundName, pathType);
            ast::TreeWalk::apply(ctx, walker, stat);
        }
    }
};

SORBET_PLUGIN(RspecPlugin);

} // namespace

} // namespace sorbet::plugins::rspec
