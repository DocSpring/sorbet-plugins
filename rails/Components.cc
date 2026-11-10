// Components: the methods ViewComponent defines for each `renders_one` and `renders_many` in a class body, as the app's
// ViewComponentSlots compiler, which this replaces, wrote them. Slots are untyped: a slot delegates to its component.
//
// - `renders_one :footer`: `footer` (`T.untyped`), `footer?`, `with_footer(*args, **kwargs, &block)` and
//   `with_footer_content(content)`.
// - `renders_many :actions`: `actions` (`T::Array[T.untyped]`), `actions?`, `with_action(*args, **kwargs, &block)`,
//   `with_action_content(content)` and `with_actions(collection_args = nil, &block)`, with the singular ActiveSupport
//   gives the name.
// - A polymorphic slot (`types:`): the getter and `?`, then a `with_x` and `with_x_content` for each type, named by the
//   type's `as:`, or by the slot (singular for `renders_many`) and the type: `renders_many :items, types: { link:
//   LinkComponent }` gives `with_item_link`. No collection setter.
//
// Each slot's getter and `?` go in the class's GeneratedSlotMethods module, included first, as ViewComponent defines
// them, so the class can override one and call `super`; the setters go on the class. A subclass gets its parent's
// slots by inheritance. A slot the plugin can't read is an error: a name that isn't a
// symbol or string, a second argument that isn't a component (a constant), a lambda, a class name or `types:` written
// out with symbol keys and literal `as:` names, and a call inside a block or condition.

#include "Inflector.h"
#include "Rails.h"
#include "Template.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

bool isSlotMacro(string_view name) {
    return name == "renders_one" || name == "renders_many";
}

// A slot's (or a polymorphic slot type's) setters: `with_footer` and `with_footer_content`.
string setters(string_view name) {
    return fmt::format("sig {{ params(args: T.untyped, kwargs: T.untyped, block: T.untyped).returns(T.untyped) }}\n"
                       "def with_{0}(*args, **kwargs, &block); end\n\n"
                       "sig {{ params(content: T.untyped).returns(T.untyped) }}\n"
                       "def with_{0}_content(content); end\n\n",
                       name);
}

// What ViewComponent renders a slot (or one of a polymorphic slot's types) with: a component class, a lambda, a class
// name or nothing. A constant is taken to be a component.
bool isRenderable(const RubyFile &file, const pm_node_t *node) {
    switch (PM_NODE_TYPE(node)) {
        case PM_CONSTANT_READ_NODE:
        case PM_CONSTANT_PATH_NODE:
        case PM_LAMBDA_NODE:
        case PM_STRING_NODE:
        case PM_NIL_NODE:
            return true;
        default: {
            auto call = asCall(node);
            return call != nullptr && call->receiver == nullptr && call->block != nullptr &&
                   PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE) &&
                   (callName(file, call) == "lambda" || callName(file, call) == "proc");
        }
    }
}

// A polymorphic slot's setter names (`types: { link: { renders: LinkComponent, as: :link } }`): each type's `as:`, or
// the slot's name (singular for `renders_many`) and the type's. nullopt with `error` set if the plugin can't read them.
optional<vector<string>> typeNames(const RubyFile &file, const pm_node_t *types, const string &slotName,
                                   string &error) {
    if (!PM_NODE_TYPE_P(types, PM_HASH_NODE)) {
        error = "the plugin reads `types:` written out as a hash";
        return nullopt;
    }
    auto pairs = hashArguments(reinterpret_cast<const pm_hash_node_t *>(types));
    if (!pairs.readable || !pairs.splats.empty()) {
        error = "the plugin reads `types:` with symbol keys";
        return nullopt;
    }
    vector<string> names;
    for (auto &[type, value] : pairs.keywords) {
        optional<string> as;
        if (PM_NODE_TYPE_P(value, PM_HASH_NODE)) {
            auto options = hashArguments(reinterpret_cast<const pm_hash_node_t *>(value));
            if (!options.readable || !options.splats.empty()) {
                error = fmt::format("the plugin reads a type's options with symbol keys (`{}`)", type);
                return nullopt;
            }
            for (auto &[key, option] : options.keywords) {
                if (key != "as" || PM_NODE_TYPE_P(option, PM_NIL_NODE)) {
                    continue;
                }
                as = nameLiteral(option);
                if (!as) {
                    error = fmt::format("the plugin reads a type's `as:` as a symbol or string (`{}`)", type);
                    return nullopt;
                }
            }
        } else if (!isRenderable(file, value)) {
            error =
                fmt::format("the plugin reads a type as a component, a lambda, a class name or `{{ renders: ..., as: "
                            "... }}` (`{}`)",
                            type);
            return nullopt;
        }
        names.emplace_back(as ? *as : fmt::format("{}_{}", slotName, type));
    }
    return names;
}

// The slot's methods, or nullopt with `error` set.
// A slot's methods: its setters, returned, and its getter and `?`, added to `getters`, which go in the class's
// GeneratedSlotMethods (ViewComponent defines them there, so the class can override them and call `super`).
optional<string> slotMethods(const RubyFile &file, const pm_call_node_t *call, const Inflector &inflector,
                             string &getters, string &error) {
    auto macro = callName(file, call);
    auto many = macro == "renders_many";
    auto args = splitArguments(call);
    auto name = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
    if (!name) {
        error = fmt::format("can't read this `{}`: the plugin reads a slot whose name is a symbol or string", macro);
        return nullopt;
    }
    auto declaration = fmt::format("{} :{}", macro, *name);
    auto unreadable = [&](string_view reason) {
        error = fmt::format("can't read `{}`: {}", declaration, reason);
        return nullopt;
    };
    constexpr string_view secondArgument =
        "the plugin reads a component, a lambda, a class name or `types:` after the name";
    if (args.positional.size() > 2 || (args.positional.size() == 2 && !args.keywords.empty())) {
        return unreadable(secondArgument);
    }
    // A hash after the name, in braces or not, is the slot's options.
    optional<Arguments> options;
    if (!args.keywords.empty() || !args.splats.empty() || !args.readable) {
        options = args;
    } else if (args.positional.size() == 2 && PM_NODE_TYPE_P(args.positional[1], PM_HASH_NODE)) {
        options = hashArguments(reinterpret_cast<const pm_hash_node_t *>(args.positional[1]));
    }
    auto slotGetters = fmt::format("sig {{ returns({1}) }}\ndef {0}; end\n\n"
                                   "sig {{ returns(T::Boolean) }}\ndef {0}?; end\n\n",
                                   *name, many ? "T::Array[T.untyped]" : "T.untyped");
    auto done = [&](string setters) -> optional<string> {
        getters += slotGetters;
        return setters;
    };
    string out;
    // `types:` makes it polymorphic: a pair of setters per type. ViewComponent ignores a hash's other keys.
    if (options) {
        auto types = absl::c_find_if(options->keywords, [](const auto &pair) { return pair.first == "types"; });
        if (!options->readable || !options->splats.empty() || types == options->keywords.end()) {
            return unreadable(secondArgument);
        }
        string problem;
        auto names = typeNames(file, types->second, many ? inflector.singularize(*name) : *name, problem);
        if (!names) {
            return unreadable(problem);
        }
        for (auto &setter : *names) {
            out += setters(setter);
        }
        return done(move(out));
    }
    if (args.positional.size() == 2 && !isRenderable(file, args.positional[1])) {
        return unreadable(secondArgument);
    }
    if (!many) {
        out += setters(*name);
        return done(move(out));
    }
    out += setters(inflector.singularize(*name));
    out += fmt::format("sig {{ params(collection_args: T.untyped, block: T.untyped).returns(T.untyped) }}\n"
                       "def with_{0}(collection_args = T.unsafe(nil), &block); end\n\n",
                       *name);
    return done(move(out));
}

} // namespace

void rewriteComponents(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                       const shared_ptr<RubyFile> &file) {
    optional<Inflector> inflector;
    string inflectorError;
    auto report = [&](const pm_node_t *node, const string &message) {
        auto [begin, end] = file->offsets(node);
        reportError(ctx, core::LocOffsets{begin, end}, message);
    };
    for (auto &def : definitions(file)) {
        if (!def.isClass) {
            continue;
        }
        vector<const pm_call_node_t *> slots;
        for (auto stat : def.body) {
            auto call = asCall(stat);
            if (call != nullptr && call->receiver == nullptr && isSlotMacro(callName(*file, call))) {
                slots.emplace_back(call);
                continue;
            }
            for (auto nested : macroCalls(*file, stat, isSlotMacro)) {
                report(reinterpret_cast<const pm_node_t *>(nested),
                       fmt::format("can't read `{}` here: the plugin reads it only directly in a class body",
                                   callName(*file, nested)));
            }
        }
        if (slots.empty()) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        auto loc = zeroAt(*tree);
        if (!inflector) {
            inflector = Inflector::forApp(app, inflectorError);
        }
        if (!inflectorError.empty()) {
            reportError(ctx, loc, inflectorError);
            continue;
        }
        string source;
        string getters;
        for (auto call : slots) {
            string error;
            auto methods = slotMethods(*file, call, *inflector, getters, error);
            if (!methods) {
                report(reinterpret_cast<const pm_node_t *>(call), error);
                continue;
            }
            source += *methods;
        }
        if (!getters.empty()) {
            source =
                fmt::format("include GeneratedSlotMethods\n\nmodule GeneratedSlotMethods\n{}end\n\n", getters) + source;
        }
        if (!source.empty()) {
            // First in the body, so a `private` in the class doesn't apply to them: ViewComponent's are public.
            ast::ClassDef::RHS_store methods;
            addTemplate(ctx, loc, methods, move(source), "slots");
            tree->rhs.insert(tree->rhs.begin(), make_move_iterator(methods.begin()), make_move_iterator(methods.end()));
        }
    }
}

} // namespace sorbet::plugins::rails
