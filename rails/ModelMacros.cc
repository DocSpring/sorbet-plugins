// Model macros: the methods Tapioca's ActiveRecordScope, ActiveRecordEnum and ActiveRecordStore compilers wrote for
// the `scope`, `enum` and `store_accessor` calls in a model's own code. The plugin reads them from the class bodies of
// the model and its superclasses, and from the `included` blocks of the app concerns each one includes.
//
// - `scope :name, ...`: `name` in GeneratedRelationMethods (the class and its relations) and
//   GeneratedAssociationRelationMethods, taking anything and returning the relation.
// - `enum :name, values, ...`: the class method `names` (the mapping, `T::Hash[T.any(String, Symbol), <value class>]`),
//   `<prefix><label><suffix>?` and `!` in EnumMethodsModule, and the scopes `<prefix><label><suffix>` and `not_...`,
//   named as Rails names them from the `prefix:`, `suffix:`, `scopes:` and `instance_methods:` options. The values
//   are a hash or array literal, or a constant set to one (with `.freeze` or `T.let`), in this file or another. A
//   class method of the app's that passes its first two parameters to `enum` (`def self.attr_enum(name, values,
//   prefix: true, **)` calling `enum(name, values, prefix:, **)`) declares enums too, with its own option defaults.
// - `store_accessor :store, :key, ...`: the accessor and dirty-tracking methods of each key in
//   GeneratedStoredAttributesMethods, named with its `prefix:` and `suffix:`.
//
// A macro the plugin can't read (a name or values it would have to run code to know, a call inside a block) is an
// error: the narrowed compilers in the app leave out every macro our code calls, so nothing else would write it. The
// macros a gem's code calls (geocoder's scopes, devise_invitable's) are still Tapioca's.
//
// As in Rails, an STI subclass has the scopes and enum mappings of the classes above it. Its EnumMethodsModule and
// GeneratedStoredAttributesMethods hold only what its own class declares; its parents' come through inheritance.

#include "Inflector.h"
#include "Rails.h"

#include "absl/algorithm/container.h"

#include <map>
#include <set>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_CONSTANT_DEPTH = 5;

// A literal option value.
struct Literal {
    enum class Kind { Absent, True, False, Nil, Name, Other };
    Kind kind = Kind::Absent;
    string name; // a symbol's or string's text
};

optional<string> nameLiteral(const pm_node_t *node) {
    if (node != nullptr && PM_NODE_TYPE_P(node, PM_SYMBOL_NODE)) {
        return stringValue(reinterpret_cast<const pm_symbol_node_t *>(node)->unescaped);
    }
    if (node != nullptr && PM_NODE_TYPE_P(node, PM_STRING_NODE)) {
        return stringValue(reinterpret_cast<const pm_string_node_t *>(node)->unescaped);
    }
    return nullopt;
}

Literal literal(const pm_node_t *node) {
    if (node == nullptr) {
        return {};
    }
    if (auto name = nameLiteral(node)) {
        return {Literal::Kind::Name, *name};
    }
    switch (PM_NODE_TYPE(node)) {
        case PM_TRUE_NODE:
            return {Literal::Kind::True, ""};
        case PM_FALSE_NODE:
            return {Literal::Kind::False, ""};
        case PM_NIL_NODE:
            return {Literal::Kind::Nil, ""};
        default:
            return {Literal::Kind::Other, ""};
    }
}

// A name Ruby can define with `def` and call without quoting: `status_draft` (then `?` or `!`).
bool isMethodName(string_view name) {
    if (name.empty() || isdigit(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    return absl::c_all_of(name, [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// A call's arguments: the positional ones, then the `key: value` pairs and `**` splats of a trailing keyword hash.
struct Arguments {
    vector<const pm_node_t *> positional;
    vector<pair<string, const pm_node_t *>> keywords;
    vector<const pm_assoc_splat_node_t *> splats;
    bool readable = true; // every keyword's key is a symbol
};

// The local variable a keyword value reads (`prefix: prefix`, or `prefix:` for short).
optional<string> readsLocal(const RubyFile &file, const pm_node_t *value) {
    if (value != nullptr && PM_NODE_TYPE_P(value, PM_IMPLICIT_NODE)) {
        value = reinterpret_cast<const pm_implicit_node_t *>(value)->value;
    }
    if (value != nullptr && PM_NODE_TYPE_P(value, PM_LOCAL_VARIABLE_READ_NODE)) {
        return string(file.name(reinterpret_cast<const pm_local_variable_read_node_t *>(value)->name));
    }
    return nullopt;
}

Arguments splitArguments(const pm_call_node_t *call) {
    Arguments out;
    for (auto arg : arguments(call)) {
        if (!PM_NODE_TYPE_P(arg, PM_KEYWORD_HASH_NODE)) {
            out.positional.emplace_back(arg);
            continue;
        }
        auto &elements = reinterpret_cast<const pm_keyword_hash_node_t *>(arg)->elements;
        for (size_t i = 0; i < elements.size; i++) {
            auto element = elements.nodes[i];
            if (PM_NODE_TYPE_P(element, PM_ASSOC_SPLAT_NODE)) {
                out.splats.emplace_back(reinterpret_cast<const pm_assoc_splat_node_t *>(element));
                continue;
            }
            auto assoc = reinterpret_cast<const pm_assoc_node_t *>(element);
            auto key = PM_NODE_TYPE_P(element, PM_ASSOC_NODE) && PM_NODE_TYPE_P(assoc->key, PM_SYMBOL_NODE)
                           ? nameLiteral(assoc->key)
                           : nullopt;
            if (!key) {
                out.readable = false;
                continue;
            }
            out.keywords.emplace_back(*key, assoc->value);
        }
    }
    return out;
}

// A class method of the app's that declares an enum: one that passes its first two parameters to `enum`.
struct EnumMacro {
    struct Option {
        optional<string> parameter; // the macro's keyword parameter that sets it
        Literal value;              // that parameter's default, or the value the macro always passes
    };
    map<string, Option, less<>> options; // by `enum`'s option name
    set<string, less<>> parameters;      // the macro's keyword parameters
    bool passesRest = false;             // `**`: the caller's other options go on to `enum`
    optional<string> problem;            // why the plugin can't read the macro
};

// `def self.<name>(a, b, ...)` with a statement `enum(a, b, ...)`, or nullopt for any other method.
optional<EnumMacro> readEnumMacro(const RubyFile &file, const pm_def_node_t *def) {
    if (!isSelf(def->receiver) || def->parameters == nullptr || def->parameters->requireds.size < 2) {
        return nullopt;
    }
    auto params = def->parameters;
    string first, second;
    for (size_t i = 0; i < 2; i++) {
        auto param = params->requireds.nodes[i];
        if (!PM_NODE_TYPE_P(param, PM_REQUIRED_PARAMETER_NODE)) {
            return nullopt;
        }
        (i == 0 ? first : second) = file.name(reinterpret_cast<const pm_required_parameter_node_t *>(param)->name);
    }
    const pm_call_node_t *call = nullptr;
    for (auto stat : statements(def->body)) {
        auto candidate = asCall(stat);
        if (candidate == nullptr || candidate->receiver != nullptr || callName(file, candidate) != "enum") {
            continue;
        }
        auto args = splitArguments(candidate);
        if (args.positional.size() >= 2 && readsLocal(file, args.positional[0]) == first &&
            readsLocal(file, args.positional[1]) == second) {
            call = candidate;
            break;
        }
    }
    if (call == nullptr) {
        return nullopt;
    }

    EnumMacro macro;
    map<string, Literal, less<>> defaults;
    for (size_t i = 0; i < params->keywords.size; i++) {
        auto keyword = params->keywords.nodes[i];
        if (PM_NODE_TYPE_P(keyword, PM_OPTIONAL_KEYWORD_PARAMETER_NODE)) {
            auto optional = reinterpret_cast<const pm_optional_keyword_parameter_node_t *>(keyword);
            auto name = string(file.name(optional->name));
            defaults[name] = literal(optional->value);
            macro.parameters.insert(name);
        } else if (PM_NODE_TYPE_P(keyword, PM_REQUIRED_KEYWORD_PARAMETER_NODE)) {
            macro.parameters.insert(
                string(file.name(reinterpret_cast<const pm_required_keyword_parameter_node_t *>(keyword)->name)));
        }
    }
    // A named `**options` (an anonymous `**` has no name).
    optional<string> rest;
    if (params->keyword_rest != nullptr && PM_NODE_TYPE_P(params->keyword_rest, PM_KEYWORD_REST_PARAMETER_NODE)) {
        auto name = reinterpret_cast<const pm_keyword_rest_parameter_node_t *>(params->keyword_rest)->name;
        if (name != 0) {
            rest = string(file.name(name));
        }
    }

    auto args = splitArguments(call);
    if (args.positional.size() != 2 || !args.readable) {
        macro.problem = "it passes `enum` more than a name, values and options";
    }
    for (auto &[key, value] : args.keywords) {
        auto local = readsLocal(file, value);
        if (local && macro.parameters.contains(*local)) {
            macro.options[key] = EnumMacro::Option{local, defaults[*local]};
            continue;
        }
        auto fixed = literal(value);
        if (fixed.kind == Literal::Kind::Other) {
            macro.problem = fmt::format("it passes `{}:` a value that isn't a literal or one of its parameters", key);
        }
        macro.options[key] = EnumMacro::Option{nullopt, fixed};
    }
    for (auto splat : args.splats) {
        if (splat->value == nullptr || (rest && readsLocal(file, splat->value) == rest)) {
            macro.passesRest = true;
        } else {
            macro.problem = "it passes `enum` options other than its own `**`";
        }
    }
    return macro;
}

// `NAME = value` among a definition's body statements.
const pm_node_t *assignedValue(const Definition &def, string_view name) {
    for (auto stat : def.body) {
        if (PM_NODE_TYPE_P(stat, PM_CONSTANT_WRITE_NODE)) {
            auto write = reinterpret_cast<const pm_constant_write_node_t *>(stat);
            if (def.file->name(write->name) == name) {
                return write->value;
            }
        }
    }
    return nullptr;
}

struct Located {
    Definition home;
    const pm_node_t *node;
};

// The value a constant was set to, and the definition whose body set it: a name looked up in the class and module
// bodies around `home`, innermost first, or a qualified name's last part in the body of the module the rest names.
optional<Located> constantValue(const App &app, const Definition &home, const ConstantRef &ref) {
    if (ref.name.size() == 1 && !ref.absolute) {
        for (auto &scope : home.nesting) {
            auto def = scope == home.fullName ? optional<Definition>(home) : resolve(app, {}, ConstantRef{scope, true});
            if (!def) {
                continue;
            }
            if (auto value = assignedValue(*def, ref.name[0])) {
                return Located{*def, value};
            }
        }
        return nullopt;
    }
    auto def =
        resolve(app, home.nesting, ConstantRef{ConstantName(ref.name.begin(), ref.name.end() - 1), ref.absolute});
    if (!def) {
        return nullopt;
    }
    if (auto value = assignedValue(*def, ref.name.back())) {
        return Located{*def, value};
    }
    return nullopt;
}

// An enum's labels, and the classes of its values (as Tapioca names them, each once, in order).
struct Values {
    vector<string> labels;
    vector<string> classes;
};

optional<string> valueClass(const pm_node_t *node) {
    switch (PM_NODE_TYPE(node)) {
        case PM_INTEGER_NODE:
            return "Integer";
        case PM_STRING_NODE:
            return "String";
        case PM_SYMBOL_NODE:
            return "Symbol";
        default:
            return nullopt;
    }
}

optional<Values> readValues(const App &app, const Definition &home, const pm_node_t *node, string_view enumName,
                            int depth, string &error) {
    auto unreadable = [&]() -> optional<Values> {
        error = fmt::format("can't read the values of `enum :{}`: the plugin reads a hash or array literal (of "
                            "symbols, strings and integers), or a constant set to one",
                            enumName);
        return nullopt;
    };
    if (depth > MAX_CONSTANT_DEPTH) {
        return unreadable();
    }
    Values values;
    auto addClass = [&](const string &name) {
        if (!absl::c_linear_search(values.classes, name)) {
            values.classes.emplace_back(name);
        }
    };
    switch (PM_NODE_TYPE(node)) {
        case PM_HASH_NODE: {
            auto &elements = reinterpret_cast<const pm_hash_node_t *>(node)->elements;
            for (size_t i = 0; i < elements.size; i++) {
                if (!PM_NODE_TYPE_P(elements.nodes[i], PM_ASSOC_NODE)) {
                    return unreadable();
                }
                auto assoc = reinterpret_cast<const pm_assoc_node_t *>(elements.nodes[i]);
                auto label = nameLiteral(assoc->key);
                auto type = valueClass(assoc->value);
                if (!label || !type) {
                    return unreadable();
                }
                values.labels.emplace_back(*label);
                addClass(*type);
            }
            return values;
        }
        case PM_ARRAY_NODE: {
            auto &elements = reinterpret_cast<const pm_array_node_t *>(node)->elements;
            for (size_t i = 0; i < elements.size; i++) {
                auto label = nameLiteral(elements.nodes[i]);
                if (!label) {
                    return unreadable();
                }
                values.labels.emplace_back(*label);
                addClass("Integer");
            }
            return values;
        }
        case PM_CALL_NODE: {
            auto call = reinterpret_cast<const pm_call_node_t *>(node);
            auto name = callName(*home.file, call);
            auto args = arguments(call);
            if (name == "freeze" && call->receiver != nullptr && args.empty()) {
                return readValues(app, home, call->receiver, enumName, depth + 1, error);
            }
            auto receiver = call->receiver ? constantRef(*home.file, call->receiver) : nullopt;
            if (name == "let" && receiver && join(receiver->name) == "T" && !args.empty()) {
                return readValues(app, home, args[0], enumName, depth + 1, error);
            }
            return unreadable();
        }
        default: {
            auto ref = constantRef(*home.file, node);
            if (!ref) {
                return unreadable();
            }
            auto found = constantValue(app, home, *ref);
            if (!found) {
                error = fmt::format("can't find the constant `{}{}` (the values of `enum :{}`): the plugin looks for "
                                    "`NAME = ...` in the bodies of the classes and modules around the call, or for a "
                                    "qualified name in the module it names",
                                    ref->absolute ? "::" : "", join(ref->name), enumName);
                return nullopt;
            }
            return readValues(app, found->home, found->node, enumName, depth + 1, error);
        }
    }
}

struct Enum {
    string name;
    vector<string> methodNames; // `<prefix><label><suffix>`, for each label
    vector<string> valueClasses;
    bool scopes = true;
    bool instanceMethods = true;
};

// What one class body declares, with what its concerns' `included` blocks declare in it.
struct ClassMacros {
    vector<string> scopes;
    vector<Enum> enums;
    vector<string> storeAccessors;
};

// Statements that run in a class body: its own, or an `included` block's, with the definition they're written in
// (where their constants are looked up).
struct Source {
    Definition home;
    vector<const pm_node_t *> statements;
};

class Reader {
public:
    Reader(const App &app, const map<string, EnumMacro, less<>> &enumMacros, vector<MacroProblem> &problems)
        : app(app), enumMacros(enumMacros), problems(problems) {}

    ClassMacros read(const Definition &def) {
        ClassMacros out;
        vector<Source> sources{{def, def.body}};
        for (auto &concern : includedConcerns(app, def)) {
            for (auto block : concern.includedBlocks()) {
                sources.emplace_back(Source{concern, statements(block->body)});
            }
        }
        for (auto &source : sources) {
            for (auto stat : source.statements) {
                statement(source.home, stat, out);
            }
        }
        return out;
    }

private:
    const App &app;
    const map<string, EnumMacro, less<>> &enumMacros;
    vector<MacroProblem> &problems;

    void problem(const Definition &home, const pm_node_t *node, string message) {
        problems.emplace_back(MacroProblem{home.file, node, move(message)});
    }

    void statement(const Definition &home, const pm_node_t *stat, ClassMacros &out) {
        auto call = asCall(stat);
        auto name = call != nullptr && call->receiver == nullptr ? callName(*home.file, call) : "";
        if (name == "scope") {
            auto args = arguments(call);
            auto scope = args.empty() ? nullopt : nameLiteral(args[0]);
            if (!scope || !isMethodName(*scope)) {
                problem(home, stat,
                        "can't read this `scope`: the plugin reads a scope whose name is a symbol or string");
                return;
            }
            out.scopes.emplace_back(*scope);
        } else if (name == "enum") {
            enumCall(home, call, nullptr, out);
        } else if (auto macro = enumMacros.find(name); !name.empty() && macro != enumMacros.end()) {
            enumCall(home, call, &macro->second, out);
        } else if (name == "store_accessor") {
            storeAccessor(home, call, out);
        } else {
            nested(home, stat);
        }
    }

    // A macro call inside another statement (a block, an `if`): Rails runs it, but the plugin doesn't read it.
    void nested(const Definition &home, const pm_node_t *stat) {
        struct Search {
            const RubyFile *file;
            const map<string, EnumMacro, less<>> *enumMacros;
            vector<const pm_call_node_t *> found;
        } search{home.file.get(), &enumMacros, {}};
        pm_visit_node(
            stat,
            [](const pm_node_t *node, void *data) -> bool {
                auto search = static_cast<Search *>(data);
                switch (PM_NODE_TYPE(node)) {
                    case PM_DEF_NODE:
                    case PM_CLASS_NODE:
                    case PM_MODULE_NODE:
                    case PM_SINGLETON_CLASS_NODE:
                    case PM_LAMBDA_NODE:
                        return false;
                    default:
                        break;
                }
                auto call = asCall(node);
                if (call != nullptr && call->receiver == nullptr) {
                    auto name = callName(*search->file, call);
                    if (name == "scope" || name == "enum" || name == "store_accessor" ||
                        search->enumMacros->contains(name)) {
                        search->found.emplace_back(call);
                    }
                }
                return true;
            },
            &search);
        for (auto call : search.found) {
            problem(home, reinterpret_cast<const pm_node_t *>(call),
                    fmt::format("can't read `{}` here: the plugin reads it only directly in a class body or a "
                                "concern's `included` block",
                                callName(*home.file, call)));
        }
    }

    void enumCall(const Definition &home, const pm_call_node_t *call, const EnumMacro *macro, ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto macroName = callName(*home.file, call);
        auto args = splitArguments(call);
        auto name = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
        if (!name || args.positional.size() != 2 || !args.readable || !args.splats.empty()) {
            problem(home, node,
                    fmt::format("can't read this `{}`: the plugin reads a name that's a symbol or string, the values, "
                                "and options written out (`key: value`)",
                                macroName));
            return;
        }
        if (macro != nullptr && macro->problem) {
            problem(home, node, fmt::format("can't read `{}`'s definition: {}", macroName, *macro->problem));
            return;
        }

        // The options `enum` gets.
        map<string, Literal, less<>> options;
        map<string, Literal, less<>> given;
        for (auto &[key, value] : args.keywords) {
            given[key] = literal(value);
        }
        if (macro == nullptr) {
            options = given;
        } else {
            for (auto &[key, option] : macro->options) {
                auto set = option.parameter ? given.find(*option.parameter) : given.end();
                options[key] = set != given.end() ? set->second : option.value;
            }
            if (macro->passesRest) {
                for (auto &[key, value] : given) {
                    if (!macro->parameters.contains(key)) {
                        options[key] = value;
                    }
                }
            }
        }
        auto option = [&](string_view key) {
            auto found = options.find(key);
            return found == options.end() ? Literal{} : found->second;
        };

        string error;
        auto affix = [&](string_view key, bool before) -> optional<string> {
            auto value = option(key);
            switch (value.kind) {
                case Literal::Kind::Absent:
                case Literal::Kind::Nil:
                case Literal::Kind::False:
                    return "";
                case Literal::Kind::True:
                    return before ? *name + "_" : "_" + *name;
                case Literal::Kind::Name:
                    return before ? value.name + "_" : "_" + value.name;
                case Literal::Kind::Other:
                    break;
            }
            error = fmt::format("can't read `{}:` of `enum :{}`: the plugin reads `true`, `false`, `nil`, a symbol or "
                                "a string",
                                key, *name);
            return nullopt;
        };
        auto flag = [&](string_view key) -> optional<bool> {
            auto value = option(key);
            if (value.kind == Literal::Kind::Absent || value.kind == Literal::Kind::True) {
                return true;
            }
            if (value.kind == Literal::Kind::False) {
                return false;
            }
            error = fmt::format("can't read `{}:` of `enum :{}`: the plugin reads `true` or `false`", key, *name);
            return nullopt;
        };
        auto prefix = affix("prefix", true);
        auto suffix = affix("suffix", false);
        auto scopes = flag("scopes");
        auto instanceMethods = flag("instance_methods");
        auto values = readValues(app, home, args.positional[1], *name, 0, error);
        if (!error.empty()) {
            problem(home, node, error);
            return;
        }

        Enum result{*name, {}, values->classes, *scopes, *instanceMethods};
        for (auto &label : values->labels) {
            auto method = *prefix + label + *suffix;
            if (!isMethodName(method)) {
                problem(home, node,
                        fmt::format("`{}` isn't a method name: the plugin writes the methods of enum labels that are "
                                    "(letters, digits and underscores)",
                                    label));
                return;
            }
            result.methodNames.emplace_back(move(method));
        }
        out.enums.emplace_back(move(result));
    }

    void storeAccessor(const Definition &home, const pm_call_node_t *call, ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto args = splitArguments(call);
        auto unreadable = [&]() {
            problem(home, node,
                    "can't read this `store_accessor`: the plugin reads a store and keys that are symbols or strings, "
                    "and `prefix:` and `suffix:` that are `true`, `false`, `nil`, a symbol or a string");
        };
        auto store = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
        if (!store || !args.readable || !args.splats.empty()) {
            unreadable();
            return;
        }
        vector<string> keys;
        for (size_t i = 1; i < args.positional.size(); i++) {
            auto arg = args.positional[i];
            vector<const pm_node_t *> elements{arg};
            if (PM_NODE_TYPE_P(arg, PM_ARRAY_NODE)) {
                auto &list = reinterpret_cast<const pm_array_node_t *>(arg)->elements;
                elements.assign(list.nodes, list.nodes + list.size);
            }
            for (auto element : elements) {
                auto key = nameLiteral(element);
                if (!key) {
                    unreadable();
                    return;
                }
                keys.emplace_back(*key);
            }
        }
        string prefix, suffix;
        for (auto &[key, value] : args.keywords) {
            if (key != "prefix" && key != "suffix") {
                continue;
            }
            auto option = literal(value);
            string affix;
            if (option.kind == Literal::Kind::True) {
                affix = *store;
            } else if (option.kind == Literal::Kind::Name) {
                affix = option.name;
            } else if (option.kind == Literal::Kind::Other) {
                unreadable();
                return;
            }
            if (!affix.empty()) {
                (key == "prefix" ? prefix : suffix) = key == "prefix" ? affix + "_" : "_" + affix;
            }
        }
        for (auto &key : keys) {
            auto accessor = prefix + key + suffix;
            if (!isMethodName(accessor)) {
                unreadable();
                return;
            }
            out.storeAccessors.emplace_back(move(accessor));
        }
    }
};

void addUnique(vector<string> &list, set<string> &seen, const string &name) {
    if (seen.insert(name).second) {
        list.emplace_back(name);
    }
}

} // namespace

string modelMacros(const App &app, const Model &model, const Inflector &inflector, vector<MacroProblem> &problems) {
    // The app's enum macros, from the class methods of the model and its superclasses.
    map<string, EnumMacro, less<>> enumMacros;
    for (auto &def : model.chain) {
        for (auto stat : def.body) {
            if (PM_NODE_TYPE_P(stat, PM_DEF_NODE)) {
                auto method = reinterpret_cast<const pm_def_node_t *>(stat);
                if (auto macro = readEnumMacro(*def.file, method)) {
                    enumMacros.emplace(def.file->name(method->name), move(*macro));
                }
            }
        }
    }

    Reader reader(app, enumMacros, problems);
    vector<ClassMacros> classes;
    for (auto &def : model.chain) {
        classes.emplace_back(reader.read(def));
    }

    // Scopes and enum mappings: the whole chain's. Enum methods and store accessors: the model's own.
    vector<string> scopes;
    set<string> seenScopes;
    vector<const Enum *> enums;
    for (auto &declared : classes) {
        for (auto &scope : declared.scopes) {
            addUnique(scopes, seenScopes, scope);
        }
        for (auto &declaredEnum : declared.enums) {
            enums.emplace_back(&declaredEnum);
            if (declaredEnum.scopes) {
                for (auto &method : declaredEnum.methodNames) {
                    addUnique(scopes, seenScopes, method);
                    addUnique(scopes, seenScopes, "not_" + method);
                }
            }
        }
    }
    auto &own = classes.front();

    string out;
    auto relationMethods = [&](string_view module, string_view relation) {
        out += fmt::format("module {}\n", module);
        for (auto &scope : scopes) {
            out += fmt::format("  sig {{ params(args: T.untyped, blk: T.untyped).returns({}) }}\n"
                               "  def {}(*args, &blk); end\n\n",
                               relation, scope);
        }
        out += "end\n\n";
    };
    if (!scopes.empty()) {
        relationMethods("GeneratedRelationMethods", "PrivateRelation");
        relationMethods("GeneratedAssociationRelationMethods", "PrivateAssociationRelation");
    }

    if (!enums.empty()) {
        out += "include EnumMethodsModule\n\nclass << self\n";
        for (auto declared : enums) {
            auto &classes = declared->valueClasses;
            auto valueType = classes.size() == 1 ? classes[0] : fmt::format("T.any({})", fmt::join(classes, ", "));
            out += fmt::format("  sig {{ returns(T::Hash[T.any(String, Symbol), {}]) }}\n  def {}; end\n\n", valueType,
                               inflector.pluralize(declared->name));
        }
        out += "end\n\nmodule EnumMethodsModule\n";
        for (auto &declared : own.enums) {
            if (!declared.instanceMethods) {
                continue;
            }
            for (auto &method : declared.methodNames) {
                out += fmt::format("  sig {{ void }}\n  def {0}!; end\n\n"
                                   "  sig {{ returns(T::Boolean) }}\n  def {0}?; end\n\n",
                                   method);
            }
        }
        out += "end\n\n";
    }

    if (!own.storeAccessors.empty()) {
        out += "include GeneratedStoredAttributesMethods\n\nmodule GeneratedStoredAttributesMethods\n";
        for (auto &key : own.storeAccessors) {
            out += fmt::format("  sig {{ params(value: T.untyped).returns(T.untyped) }}\n  def {0}=(value); end\n\n"
                               "  sig {{ returns(T.untyped) }}\n  def {0}; end\n\n"
                               "  sig {{ returns(T::Boolean) }}\n  def {0}_changed?; end\n\n"
                               "  sig {{ returns(T.untyped) }}\n  def {0}_change; end\n\n"
                               "  sig {{ returns(T.untyped) }}\n  def {0}_was; end\n\n"
                               "  sig {{ returns(T::Boolean) }}\n  def saved_change_to_{0}?; end\n\n"
                               "  sig {{ returns(T.untyped) }}\n  def saved_change_to_{0}; end\n\n"
                               "  sig {{ returns(T.untyped) }}\n  def {0}_before_last_save; end\n\n",
                               key);
        }
        out += "end\n";
    }
    return out;
}

} // namespace sorbet::plugins::rails
