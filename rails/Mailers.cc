// Mailers: each action as a class method with the action's parameters and sig types, returning
// `::ActionMailer::MessageDelivery`: what ActionMailer::Base's `method_missing` answers for it. The methods Tapioca's
// ActionMailer compiler wrote, from `action_methods`: the public instance methods a mailer defines and those of the
// app modules it includes. A gem module's public methods (`ActionView::Helpers::NumberHelper`'s) are actions at
// runtime too, but nobody sends them, so they're left out. A mailer gets the actions it inherits from its parent's
// class methods, and a gem parent's (Devise::Mailer's) from that gem's RBI.

#include "Rails.h"

#include "absl/algorithm/container.h"

#include <set>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_MODULE_DEPTH = 4;

struct Action {
    string name;
    Definition owner;
    Definition::Method method;
};

// The public methods of the app modules `def` includes that the class doesn't define itself: the last module
// included first, as Ruby looks them up, and each module's own before those of the modules it includes.
void moduleActions(const App &app, const Definition &def, int depth, set<string> &seen, vector<Action> &out) {
    if (depth > MAX_MODULE_DEPTH) {
        return;
    }
    auto mixins = def.mixins("include");
    for (auto mixin = mixins.rbegin(); mixin != mixins.rend(); ++mixin) {
        auto module = resolve(app, def.nesting, *mixin);
        if (!module || module->isClass) {
            continue;
        }
        for (auto &[name, method] : module->publicMethods()) {
            if (seen.insert(name).second) {
                out.emplace_back(Action{name, *module, method});
            }
        }
        moduleActions(app, *module, depth + 1, seen, out);
    }
}

} // namespace

void rewriteMailers(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    const ConstantRef messageDelivery{{"ActionMailer", "MessageDelivery"}, true};
    for (auto &def : definitions(file)) {
        if (!def.isClass) {
            continue;
        }
        // Whether it's a mailer is in its own Zeitwerk file, whichever file reopens it.
        auto main = resolve(app, {}, ConstantRef{def.fullName, true});
        if (lineage(app, main ? *main : def).kind != Kind::Mailer) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        auto loc = zeroAt(*tree);
        // Each file that opens the class adds the actions its own body defines.
        set<string> seen;
        vector<Action> actions;
        for (auto &[name, method] : def.publicMethods()) {
            seen.insert(name);
            actions.emplace_back(Action{name, def, method});
        }
        moduleActions(app, def, 0, seen, actions);

        // A module's methods go in a module inside that module's nesting, so their sigs' constants resolve as they
        // do there, and the mailer extends it.
        vector<pair<Definition, ast::ClassDef::RHS_store>> elsewhere;
        for (auto &action : actions) {
            auto copied = readMethod(ctx, loc, action.owner, action.method,
                                     fmt::format("{}#{}", join(action.owner.fullName), action.name));
            if (!copied) {
                continue;
            }
            if (action.owner.file->path == file->path && action.owner.fullName == def.fullName) {
                addCopiedMethod(ctx, tree->rhs, loc, action.name, *copied, true, messageDelivery);
                continue;
            }
            auto entry = absl::c_find_if(
                elsewhere, [&](auto &existing) { return existing.first.fullName == action.owner.fullName; });
            if (entry == elsewhere.end()) {
                elsewhere.emplace_back(action.owner, ast::ClassDef::RHS_store{});
                entry = prev(elsewhere.end());
            }
            addCopiedMethod(ctx, entry->second, loc, action.name, *copied, false, messageDelivery);
        }
        for (auto &[owner, methods] : elsewhere) {
            auto module = addUnderNesting(ctx, root, loc, owner, "SorbetMailerActions", move(methods));
            tree->rhs.emplace_back(mixinStatement(ctx, loc, "extend", module));
        }
    }
}

} // namespace sorbet::plugins::rails
