// Helpers: each controller's and mailer's `HelperMethods`, the module its views get their helpers from, as Rails
// builds it (AbstractController::Helpers) from what the app's code says. Controllers also get `helpers`, which
// returns `HelperProxy`: an ActionView::Base that includes `HelperMethods`. These are the modules Tapioca's helper
// compilers wrote; what gems add to them at runtime (a gem's `helper_method`, the helper modules of engines) stays
// in those compilers' RBIs.
//
// A class in app/controllers or app/mailers whose superclass chain reaches ActionController::Base or
// ActionMailer::Base (or a gem's controller or mailer) gets, from its Zeitwerk file:
// - `include ::<Parent>::HelperMethods` when its superclass is an app class;
// - its default helper module, `<Name>Helper` without `Controller` (`ApplicationHelper` for ApplicationController),
//   when the app has one its ancestors don't include;
// - directly under ActionController::Base, every module in app/helpers, in Rails' order (`helper :all`).
// Each file that opens the class adds what its body says: `helper SomeHelper` includes the module and
// `helper_method :name` adds a method with the parameters and sig of the method it names. So do the `included`
// blocks of the concerns it includes, unless an ancestor already includes them (Rails runs them once).
//
// Every such class gets a `HelperMethods`, whether or not Rails gives it one of its own (without, Rails uses the
// parent's), so the erb_views generator can name any class's.

#include "Rails.h"

#include "absl/algorithm/container.h"
#include "common/sort/sort.h"

#include <cstring>
#include <set>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_DEPTH = 10;
constexpr string_view HELPER_DIR = "app/helpers/";

ConstantName withLast(ConstantName name, string last) {
    name.back() = move(last);
    return name;
}

// What `helper_method :name` names: the method and where the call is.
struct HelperMethod {
    string name;
    shared_ptr<RubyFile> file;
    const pm_node_t *node;
};

// What builds a class's helpers, gathered from the statements that build them.
struct Gathered {
    // The app's helper modules included so far (full names joined), by the class or its app ancestors.
    set<string> modules;
    // The app's concerns included so far, by the class or its app ancestors.
    set<string> concerns;
    // What this class adds (nullptr while gathering what its ancestors did).
    vector<ConstantName> *includes = nullptr;
    vector<HelperMethod> *methods = nullptr;
};

struct Walker {
    core::MutableContext ctx;
    const App &app;
    const shared_ptr<RubyFile> &current; // the file Sorbet is rewriting
    core::LocOffsets classLoc;

    // Where to report a problem with `node` in `file`: the node itself in the file being rewritten, else the class.
    core::LocOffsets locOf(const RubyFile &file, const pm_node_t *node) const {
        if (file.path != current->path) {
            return classLoc;
        }
        auto [begin, end] = file.offsets(node);
        return core::LocOffsets{begin, end};
    }

    void error(const RubyFile &file, const pm_node_t *node, string_view message) const {
        auto at = file.path == current->path ? string() : fmt::format(" ({}:{})", file.path, file.line(node));
        reportError(ctx, locOf(file, node), fmt::format("{}{}", message, at));
    }

    void addModule(Gathered &gathered, const ConstantName &name) const {
        if (gathered.modules.insert(join(name)).second && gathered.includes != nullptr) {
            gathered.includes->emplace_back(name);
        }
    }

    // The module a helper file defines, by its path under app/helpers (`active_admin/views_helper.rb` defines
    // `ActiveAdmin::ViewsHelper`). Rails turns the path into the constant with the app's inflections; matching names
    // ignoring case and underscores finds the same one.
    optional<ConstantName> helperModuleAt(const string &relative) const {
        auto path = app.find(string(HELPER_DIR) + relative);
        auto file = path ? RubyFile::read(*path) : nullptr;
        if (file == nullptr) {
            return nullopt;
        }
        vector<string> want;
        string stem = relative.substr(0, relative.size() - 3);
        for (size_t start = 0;;) {
            auto slash = stem.find('/', start);
            want.emplace_back(normalizedName(stem.substr(start, slash - start)));
            if (slash == string::npos) {
                break;
            }
            start = slash + 1;
        }
        for (auto &def : definitions(file)) {
            if (def.isClass || def.fullName.size() != want.size()) {
                continue;
            }
            bool same = true;
            for (size_t i = 0; i < want.size() && same; i++) {
                same = normalizedName(def.fullName[i]) == want[i];
            }
            if (same) {
                return def.fullName;
            }
        }
        return nullopt;
    }

    // `helper :all`: every `*_helper.rb` under app/helpers, sorted by its path without `_helper.rb`, as Rails sorts
    // them (AbstractController::Helpers.all_helpers_from_path).
    vector<string> allHelperFiles() const {
        vector<pair<string, string>> found; // (sort key, path under app/helpers)
        vector<string> dirs = {""};
        while (!dirs.empty()) {
            auto dir = dirs.back();
            dirs.pop_back();
            for (auto &entry : app.list(string(HELPER_DIR) + dir)) {
                auto relative = dir + entry;
                if (app.isDirectory(string(HELPER_DIR) + relative)) {
                    dirs.emplace_back(relative + "/");
                } else if (endsWith(relative, "_helper.rb")) {
                    found.emplace_back(relative.substr(0, relative.size() - strlen("_helper.rb")), relative);
                }
            }
        }
        fast_sort(found);
        vector<string> out;
        for (auto &[key, path] : found) {
            out.emplace_back(path);
        }
        return out;
    }

    void addAllHelpers(Gathered &gathered, const RubyFile &file, const pm_node_t *node) const {
        for (auto &relative : allHelperFiles()) {
            if (auto name = helperModuleAt(relative)) {
                addModule(gathered, *name);
            } else {
                error(file, node,
                      fmt::format("app/helpers/{} doesn't define the module its path names, which `helper :all` "
                                  "includes",
                                  relative));
            }
        }
    }

    // The default helper module Rails includes in every named class: `<Name>Helper`, without `Controller`.
    void addDefaultHelper(Gathered &gathered, const Definition &def) const {
        auto last = def.fullName.back();
        if (endsWith(last, "Controller")) {
            last = last.substr(0, last.size() - strlen("Controller"));
        }
        auto found = resolve(app, {}, ConstantRef{withLast(def.fullName, last + "Helper"), true});
        if (found && !found->isClass) {
            addModule(gathered, found->fullName);
        }
    }

    // `helper` with modules (an app's: a gem's comes from the generated RBI), `:all`, or helper names.
    void helperCall(Gathered &gathered, const Definition &scope, const pm_call_node_t *call) const {
        auto &file = *scope.file;
        if (call->block != nullptr) {
            error(file, reinterpret_cast<const pm_node_t *>(call),
                  "a `helper` block isn't typed: define its methods in a module in app/helpers and pass that");
            return;
        }
        for (auto arg : arguments(call)) {
            if (auto ref = constantRef(file, arg)) {
                auto found = resolve(app, scope.nesting, *ref);
                if (found && !found->isClass) {
                    addModule(gathered, found->fullName);
                }
                continue;
            }
            string name;
            if (PM_NODE_TYPE_P(arg, PM_SYMBOL_NODE)) {
                name = stringValue(reinterpret_cast<const pm_symbol_node_t *>(arg)->unescaped);
            } else if (PM_NODE_TYPE_P(arg, PM_STRING_NODE)) {
                name = stringValue(reinterpret_cast<const pm_string_node_t *>(arg)->unescaped);
            } else {
                error(file, arg, "`helper` takes modules, `:all` or helper names: the plugin can't read this one");
                continue;
            }
            if (name == "all") {
                addAllHelpers(gathered, file, arg);
            } else if (auto found = helperModuleAt(name + "_helper.rb")) {
                addModule(gathered, *found);
            } else {
                error(file, arg, fmt::format("can't find app/helpers/{}_helper.rb, which `helper` names", name));
            }
        }
    }

    void helperMethodCall(Gathered &gathered, const Definition &scope, const pm_call_node_t *call) const {
        for (auto arg : arguments(call)) {
            string name;
            if (PM_NODE_TYPE_P(arg, PM_SYMBOL_NODE)) {
                name = stringValue(reinterpret_cast<const pm_symbol_node_t *>(arg)->unescaped);
            } else if (PM_NODE_TYPE_P(arg, PM_STRING_NODE)) {
                name = stringValue(reinterpret_cast<const pm_string_node_t *>(arg)->unescaped);
            } else {
                error(*scope.file, arg, "`helper_method` takes method names: the plugin can't read this one");
                continue;
            }
            if (gathered.methods != nullptr) {
                gathered.methods->emplace_back(HelperMethod{name, scope.file, arg});
            }
        }
    }

    static bool isConcern(const Definition &def) {
        for (auto &ref : def.mixins("extend")) {
            if (join(ref.name) == "ActiveSupport::Concern") {
                return true;
            }
        }
        return false;
    }

    // What including an app concern does to the helpers: its own concerns first (ActiveSupport::Concern includes a
    // concern's dependencies in the class that includes it), then its `included` block.
    void includeConcern(Gathered &gathered, const Definition &concern, int depth) const {
        if (depth > MAX_DEPTH || !isConcern(concern) || !gathered.concerns.insert(join(concern.fullName)).second) {
            return;
        }
        for (auto stat : concern.body) {
            if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
                continue;
            }
            auto call = reinterpret_cast<const pm_call_node_t *>(stat);
            if (call->receiver != nullptr) {
                continue;
            }
            auto name = callName(*concern.file, call);
            if (name == "include") {
                includeModules(gathered, concern, call, depth + 1);
            } else if (name == "included" && call->block != nullptr && PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
                auto block = reinterpret_cast<const pm_block_node_t *>(call->block);
                walk(gathered, concern, statements(block->body), depth + 1);
            }
        }
    }

    // `include A, B` runs B's hooks first, as Ruby does.
    void includeModules(Gathered &gathered, const Definition &scope, const pm_call_node_t *call, int depth) const {
        auto args = arguments(call);
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto ref = constantRef(*scope.file, *it);
            if (!ref) {
                continue;
            }
            auto found = resolve(app, scope.nesting, *ref);
            if (found && !found->isClass) {
                includeConcern(gathered, *found, depth);
            }
        }
    }

    // The statements of a class body or a concern's `included` block, in order.
    void walk(Gathered &gathered, const Definition &scope, const vector<const pm_node_t *> &stats, int depth) const {
        for (auto stat : stats) {
            if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
                continue;
            }
            auto call = reinterpret_cast<const pm_call_node_t *>(stat);
            if (call->receiver != nullptr) {
                continue;
            }
            auto name = callName(*scope.file, call);
            if (name == "include") {
                includeModules(gathered, scope, call, depth);
            } else if (name == "helper") {
                helperCall(gathered, scope, call);
            } else if (name == "helper_method") {
                helperMethodCall(gathered, scope, call);
            }
        }
    }

    // What a class's Zeitwerk file gives its helpers before its body: the default helper and `helper :all`.
    void classStart(Gathered &gathered, const Definition &def, bool allHelpers) const {
        addDefaultHelper(gathered, def);
        if (allHelpers) {
            addAllHelpers(gathered, *def.file, def.node);
        }
    }
};

// Where Ruby finds an instance method of a class: in the class (`selves`: its definitions, this file's first), in
// the modules it includes (the last one first, then the modules they include), then the same in each superclass.
struct Found {
    Definition owner;
    Definition::Method method;
};

optional<Found> findInModule(const App &app, const Definition &module, string_view name, int depth) {
    if (auto method = module.method(name)) {
        return Found{module, *method};
    }
    if (depth > MAX_DEPTH) {
        return nullopt;
    }
    auto mixins = module.mixins("include");
    for (auto it = mixins.rbegin(); it != mixins.rend(); ++it) {
        auto found = resolve(app, module.nesting, *it);
        if (found && !found->isClass) {
            if (auto inner = findInModule(app, *found, name, depth + 1)) {
                return inner;
            }
        }
    }
    return nullopt;
}

optional<Found> findInMixins(const App &app, const Definition &def, string_view name) {
    auto mixins = def.mixins("include");
    for (auto it = mixins.rbegin(); it != mixins.rend(); ++it) {
        auto found = resolve(app, def.nesting, *it);
        if (found && !found->isClass) {
            if (auto inner = findInModule(app, *found, name, 0)) {
                return inner;
            }
        }
    }
    return nullopt;
}

optional<Found> findMethod(const App &app, const vector<const Definition *> &selves, const vector<Definition> &parents,
                           string_view name) {
    for (auto self : selves) {
        if (auto method = self->method(name)) {
            return Found{*self, *method};
        }
    }
    for (auto self : selves) {
        if (auto found = findInMixins(app, *self, name)) {
            return found;
        }
    }
    for (auto &parent : parents) {
        if (auto method = parent.method(name)) {
            return Found{parent, *method};
        }
        if (auto found = findInMixins(app, parent, name)) {
            return found;
        }
    }
    return nullopt;
}

string flatName(const ConstantName &name) {
    string out;
    for (auto &part : name) {
        out += (out.empty() ? "" : "__") + part;
    }
    return out;
}

} // namespace

void rewriteHelpers(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    for (auto &def : definitions(file)) {
        if (!def.isClass) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        // The class's superclass and default helpers are in its Zeitwerk file. A class without one (a nested
        // error class) is read from here.
        auto main = resolve(app, {}, ConstantRef{def.fullName, true});
        auto isMain = !main || main->file->path == file->path;
        const Definition &mainDef = isMain ? def : *main;
        auto line = lineage(app, mainDef);
        if (line.kind == Kind::None) {
            continue;
        }
        auto loc = zeroAt(*tree);
        Walker walker{ctx, app, file, loc};

        // What the class's app ancestors gave its helpers, from the farthest down.
        Gathered gathered;
        for (auto it = line.parents.rbegin(); it != line.parents.rend(); ++it) {
            walker.classStart(gathered, *it, line.directBase && it == line.parents.rbegin() &&
                                                 line.kind == Kind::Controller);
            walker.walk(gathered, *it, it->body, 0);
        }
        auto allHelpers = line.directBase && line.parents.empty() && line.kind == Kind::Controller;
        if (!isMain) {
            walker.classStart(gathered, mainDef, allHelpers);
            walker.walk(gathered, mainDef, mainDef.body, 0);
        }

        vector<ConstantName> includes;
        vector<HelperMethod> methods;
        gathered.includes = &includes;
        gathered.methods = &methods;
        if (isMain) {
            walker.classStart(gathered, def, allHelpers);
        }
        walker.walk(gathered, def, def.body, 0);

        ast::ClassDef::RHS_store body;
        if (isMain && !line.parents.empty()) {
            auto parentModule = line.parents.front().fullName;
            parentModule.emplace_back("HelperMethods");
            body.emplace_back(mixinStatement(ctx, loc, "include", parentModule));
        }
        for (auto &name : includes) {
            body.emplace_back(mixinStatement(ctx, loc, "include", name));
        }

        // Methods defined in this file's opening of the class go in its HelperMethods; others in a module inside
        // their own file's nesting, so their sigs' constants resolve as they do there.
        vector<const Definition *> selves = {&def};
        if (!isMain) {
            selves.emplace_back(&mainDef);
        }
        vector<pair<Definition, ast::ClassDef::RHS_store>> elsewhere;
        for (auto &method : methods) {
            auto found = findMethod(app, selves, line.parents, method.name);
            if (!found) {
                walker.error(*method.file, method.node,
                             fmt::format("can't find the method `helper_method :{}` names", method.name));
                continue;
            }
            auto copied = readMethod(ctx, walker.locOf(*method.file, method.node), found->owner, found->method,
                                     fmt::format("{}#{}", join(found->owner.fullName), method.name));
            if (!copied) {
                continue;
            }
            if (found->owner.file->path == file->path && found->owner.fullName == def.fullName) {
                addCopiedMethod(ctx, body, loc, method.name, *copied, false);
                continue;
            }
            auto at = absl::c_find_if(elsewhere, [&](auto &entry) {
                return entry.first.file->path == found->owner.file->path &&
                       entry.first.fullName == found->owner.fullName;
            });
            if (at == elsewhere.end()) {
                elsewhere.emplace_back(found->owner, ast::ClassDef::RHS_store{});
                at = elsewhere.end() - 1;
            }
            addCopiedMethod(ctx, at->second, loc, method.name, *copied, false);
        }
        for (auto &[owner, methodsBody] : elsewhere) {
            auto module = addUnderNesting(ctx, root, loc, owner, "SorbetHelperMethodsFor" + flatName(def.fullName),
                                          move(methodsBody));
            body.emplace_back(mixinStatement(ctx, loc, "include", module));
        }

        if (isMain || !body.empty()) {
            tree->rhs.emplace_back(ast::MK::Module(
                loc, loc, constantTree(ctx, loc, ConstantRef{{"HelperMethods"}, false}), move(body)));
        }
        if (isMain && line.kind == Kind::Controller) {
            // First in the body, so a `private` in the class doesn't apply to it.
            ast::ClassDef::RHS_store helpers;
            addMethod(ctx, helpers, loc, "helpers", {}, {},
                      constantTree(ctx, loc, ConstantRef{{"HelperProxy"}, false}), false);
            tree->rhs.insert(tree->rhs.begin(), make_move_iterator(helpers.begin()),
                             make_move_iterator(helpers.end()));
            auto helperMethods = def.fullName;
            helperMethods.emplace_back("HelperMethods");
            ast::ClassDef::RHS_store proxyBody;
            proxyBody.emplace_back(mixinStatement(ctx, loc, "include", helperMethods));
            ast::ClassDef::ANCESTORS_store ancestors;
            ancestors.emplace_back(constantTree(ctx, loc, ConstantRef{{"ActionView", "Base"}, true}));
            tree->rhs.emplace_back(ast::MK::Class(loc, loc, constantTree(ctx, loc, ConstantRef{{"HelperProxy"}, false}),
                                                  move(ancestors), move(proxyBody)));
        }
    }
}

} // namespace sorbet::plugins::rails
