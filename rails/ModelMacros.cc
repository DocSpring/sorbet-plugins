// Model macros: the methods Tapioca's ActiveRecordScope, ActiveRecordEnum and ActiveRecordStore compilers wrote for
// the `scope`, `enum` and `store_accessor` calls in a model's own code, the changes the `attribute`, `enum`,
// `serialize`, `alias_attribute` and `ignored_columns` calls make to its attributes (ModelColumns.cc writes those), and
// its associations (ModelAssociations.cc writes those). The plugin reads them from the class bodies of the model and
// its superclasses, and from the `included` blocks of the app concerns each one includes.
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
// - `attribute :name, :type` (a symbol, with `array:`), `serialize :name, coder:, type:`, `alias_attribute`,
//   `self.ignored_columns = [...]` and `+= [...]`, and a class method of the app's that gives each name in its `*`
//   parameter to `attribute` with a type (`def self.default_boolean_value(*fields, ...)`).
// - `belongs_to`, `has_one`, `has_many` and `has_and_belongs_to_many` with a literal name and literal `class_name:`,
//   `through:`, `source:`, `source_type:` and `polymorphic:` options, and `accepts_nested_attributes_for` with literal
//   names. A name that a class body and a concern's `included` block both declare, with other options, is an error.
//
// Any other class method of the app's (in the model's chain, or a concern's `class_methods` block) that calls one of
// these macros is an error where a class body calls it: the plugin can't see what it declares.
//
// A macro the plugin can't read (a name or values it would have to run code to know, a call inside a block) is an
// error: the narrowed compilers in the app leave out every macro our code calls, so nothing else would write it. The
// macros a gem's code calls (geocoder's scopes, devise_invitable's) are still Tapioca's.
//
// As in Rails, an STI subclass has the scopes and enum mappings of the classes above it. Its EnumMethodsModule and
// GeneratedStoredAttributesMethods hold only what its own class declares; its parents' come through inheritance.

#include "Inflector.h"
#include "Rails.h"
#include "Schema.h"

#include "absl/algorithm/container.h"

#include <functional>
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

// The macros Rails and ActiveRecord give a model class whose methods or types the plugin writes.
bool isModelMacro(string_view name) {
    return name == "scope" || name == "enum" || name == "store_accessor" || name == "attribute" ||
           name == "serialize" || name == "alias_attribute" || name == "belongs_to" || name == "has_one" ||
           name == "has_many" || name == "has_and_belongs_to_many" || name == "accepts_nested_attributes_for";
}

optional<Association::Macro> associationMacro(string_view name) {
    if (name == "belongs_to") {
        return Association::Macro::BelongsTo;
    }
    if (name == "has_one") {
        return Association::Macro::HasOne;
    }
    if (name == "has_many") {
        return Association::Macro::HasMany;
    }
    if (name == "has_and_belongs_to_many") {
        return Association::Macro::HasAndBelongsToMany;
    }
    return nullopt;
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
    // `attribute(name, :type, ...)` in it (`attr_enum`'s `:integer`), and whether it comes before the `enum`.
    optional<string> attributeType;
    bool attributeFirst = false;
    optional<string> problem; // why the plugin can't read the macro
};

// A class method of the app's that declares attributes: `def self.<name>(*names, ...)` whose statement
// `names.each do |name| ... end` has a statement `attribute name, :type, ...` (`default_boolean_value`).
struct AttributeMacro {
    string type;
    const pm_call_node_t *call;
};

// The `attribute(name, :type, ...)` call's type: the call's first argument reads the local `name`, its second is a
// symbol.
optional<string> attributeTypeOf(const RubyFile &file, const pm_call_node_t *call, string_view name) {
    auto args = splitArguments(call);
    if (call->receiver != nullptr || callName(file, call) != "attribute" || args.positional.size() != 2 ||
        readsLocal(file, args.positional[0]) != name || !PM_NODE_TYPE_P(args.positional[1], PM_SYMBOL_NODE) ||
        !args.readable || !args.splats.empty()) {
        return nullopt;
    }
    // `array:` and `range:` change the type.
    for (auto &[key, value] : args.keywords) {
        if (key == "array" || key == "range") {
            return nullopt;
        }
    }
    return nameLiteral(args.positional[1]);
}

optional<AttributeMacro> readAttributeMacro(const RubyFile &file, const pm_def_node_t *def, bool singleton) {
    if (singleton != isSelf(def->receiver) || def->parameters == nullptr || def->parameters->rest == nullptr ||
        !PM_NODE_TYPE_P(def->parameters->rest, PM_REST_PARAMETER_NODE)) {
        return nullopt;
    }
    auto restName = reinterpret_cast<const pm_rest_parameter_node_t *>(def->parameters->rest)->name;
    if (restName == 0) {
        return nullopt;
    }
    auto names = file.name(restName);
    for (auto stat : statements(def->body)) {
        auto each = asCall(stat);
        if (each == nullptr || callName(file, each) != "each" || readsLocal(file, each->receiver) != names ||
            each->block == nullptr || !PM_NODE_TYPE_P(each->block, PM_BLOCK_NODE)) {
            continue;
        }
        auto block = reinterpret_cast<const pm_block_node_t *>(each->block);
        if (block->parameters == nullptr || !PM_NODE_TYPE_P(block->parameters, PM_BLOCK_PARAMETERS_NODE)) {
            continue;
        }
        auto params = reinterpret_cast<const pm_block_parameters_node_t *>(block->parameters)->parameters;
        if (params == nullptr || params->requireds.size != 1 ||
            !PM_NODE_TYPE_P(params->requireds.nodes[0], PM_REQUIRED_PARAMETER_NODE)) {
            continue;
        }
        auto name = file.name(reinterpret_cast<const pm_required_parameter_node_t *>(params->requireds.nodes[0])->name);
        for (auto inner : statements(block->body)) {
            auto call = asCall(inner);
            if (call == nullptr) {
                continue;
            }
            if (auto type = attributeTypeOf(file, call, name)) {
                return AttributeMacro{*type, call};
            }
        }
    }
    return nullopt;
}

// `def self.<name>(a, b, ...)` with a statement `enum(a, b, ...)`, or nullopt for any other method. In a concern's
// `class_methods` block, `def <name>`.
optional<EnumMacro> readEnumMacro(const RubyFile &file, const pm_def_node_t *def, bool singleton) {
    if (singleton != isSelf(def->receiver) || def->parameters == nullptr || def->parameters->requireds.size < 2) {
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
    const pm_call_node_t *attributeCall = nullptr;
    EnumMacro macro;
    for (auto stat : statements(def->body)) {
        auto candidate = asCall(stat);
        if (candidate == nullptr || candidate->receiver != nullptr) {
            continue;
        }
        if (attributeCall == nullptr) {
            if (auto type = attributeTypeOf(file, candidate, first)) {
                attributeCall = candidate;
                macro.attributeType = type;
                macro.attributeFirst = call == nullptr;
                continue;
            }
        }
        if (call != nullptr || callName(file, candidate) != "enum") {
            continue;
        }
        auto args = splitArguments(candidate);
        if (args.positional.size() >= 2 && readsLocal(file, args.positional[0]) == first &&
            readsLocal(file, args.positional[1]) == second) {
            call = candidate;
        }
    }
    if (call == nullptr) {
        return nullopt;
    }
    for (auto other : macroCalls(file, def->body, isModelMacro)) {
        if (other != call && other != attributeCall) {
            macro.problem = fmt::format("it calls `{}` too", callName(file, other));
        }
    }

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
            return "String"; // ActiveRecord::Enum stores a Symbol value as its name.
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
        case PM_HASH_NODE:
        case PM_KEYWORD_HASH_NODE: { // `enum :status, active: 0`: its values written as keywords
            auto &elements = PM_NODE_TYPE_P(node, PM_HASH_NODE)
                                 ? reinterpret_cast<const pm_hash_node_t *>(node)->elements
                                 : reinterpret_cast<const pm_keyword_hash_node_t *>(node)->elements;
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

// A class method of the app's that calls model macros, as the plugin reads it.
struct AppMacro {
    optional<EnumMacro> enumMacro;
    optional<AttributeMacro> attributeMacro;
    optional<string> problem; // why the plugin can't read it
};

// `self.ignored_columns = [...]` (replaces its superclass's list) or `+= [...]`.
struct IgnoredColumns {
    bool replace;
    vector<string> names;
};

// `include Uploader.attachment(:name)`: a Shrine attachment, at its place in the model's chain.
struct ShrineAttachmentInclude {
    string name;
    UploaderPlugins plugins;
    size_t level;
    Definition home;       // the class, or the concern whose `included` block includes it
    const pm_node_t *stat; // the `include` statement
};

// The `Uploader.attachment(:name)` call of `include Uploader.attachment(:name)`, or of `T.unsafe(self).include ...`
// (Sorbet rejects an `include` of a call), or nullptr for any other statement.
const pm_call_node_t *attachmentInclude(const RubyFile &file, const pm_call_node_t *call) {
    if (call == nullptr || callName(file, call) != "include") {
        return nullptr;
    }
    if (call->receiver != nullptr) {
        auto unsafe = asCall(call->receiver);
        auto t = unsafe == nullptr || unsafe->receiver == nullptr ? nullopt : constantRef(file, unsafe->receiver);
        auto unsafeArgs = unsafe == nullptr ? vector<const pm_node_t *>{} : arguments(unsafe);
        if (!t || join(t->name) != "T" || callName(file, unsafe) != "unsafe" || unsafeArgs.size() != 1 ||
            !isSelf(unsafeArgs[0])) {
            return nullptr;
        }
    }
    auto args = arguments(call);
    auto attachment = args.size() == 1 ? asCall(args[0]) : nullptr;
    if (attachment == nullptr || attachment->receiver == nullptr ||
        (callName(file, attachment) != "attachment" && callName(file, attachment) != "Attachment")) {
        return nullptr;
    }
    return attachment;
}

// What one class body declares, with what its concerns' `included` blocks declare in it.
struct ClassMacros {
    vector<string> scopes;
    vector<Enum> enums;
    vector<string> storeAccessors;
    vector<AttributeChange> attributes;
    vector<string> attributeSources; // for each change, the class or concern it's written in
    vector<IgnoredColumns> ignoredColumns;
    vector<Association> associations;
    vector<string> associationSources; // the same
    vector<string> nestedAttributes;
    vector<ShrineAttachmentInclude> attachments;
};

// Statements that run in a class body: its own, or an `included` block's, with the definition they're written in
// (where their constants are looked up).
struct Source {
    Definition home;
    vector<const pm_node_t *> statements;
};

class Reader {
public:
    Reader(const App &app, const map<string, AppMacro, less<>> &macros, vector<MacroProblem> &problems)
        : app(app), macros(macros), problems(problems) {}

    // The macros of the class at `level` in the model's chain.
    ClassMacros read(const Definition &def, size_t level) {
        owner = &def;
        this->level = level;
        ClassMacros out;
        vector<Source> sources{{def, def.body}};
        auto concerns = includedConcerns(app, def);
        includedHooks(def, concerns);
        for (auto &concern : concerns) {
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
    const map<string, AppMacro, less<>> &macros;
    vector<MacroProblem> &problems;
    const Definition *owner = nullptr; // the class being read
    size_t level = 0;

    void problem(const Definition &home, const pm_node_t *node, string message) {
        problems.emplace_back(MacroProblem{home.file, node, move(message)});
    }

    void change(const Definition &home, const pm_call_node_t *call, ClassMacros &out, AttributeChange::Kind kind,
                string name, string detail = "", bool array = false) {
        out.attributes.emplace_back(AttributeChange{kind, move(name), move(detail), array, home.file,
                                                    reinterpret_cast<const pm_node_t *>(call)});
        out.attributeSources.emplace_back(join(home.fullName));
    }

    void statement(const Definition &home, const pm_node_t *stat, ClassMacros &out) {
        if (PM_NODE_TYPE_P(stat, PM_CALL_OPERATOR_WRITE_NODE)) {
            auto write = reinterpret_cast<const pm_call_operator_write_node_t *>(stat);
            if (isSelf(write->receiver) && home.file->name(write->read_name) == "ignored_columns") {
                ignoredColumns(home, stat, write->value, false, "+=", out);
                return;
            }
        }
        auto call = asCall(stat);
        if (call != nullptr && isSelf(call->receiver) && callName(*home.file, call) == "ignored_columns=") {
            auto args = arguments(call);
            ignoredColumns(home, stat, args.size() == 1 ? args[0] : nullptr, true, "=", out);
            return;
        }
        if (auto attachment = attachmentInclude(*home.file, call)) {
            attachmentCall(home, stat, attachment, out);
            return;
        }
        auto name = call != nullptr && call->receiver == nullptr ? callName(*home.file, call) : "";
        auto macro = name.empty() ? macros.end() : macros.find(name);
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
        } else if (macro != macros.end()) {
            if (macro->second.problem) {
                problem(home, stat, fmt::format("can't read `{}`'s definition: {}", name, *macro->second.problem));
            } else if (macro->second.enumMacro) {
                enumCall(home, call, &*macro->second.enumMacro, out);
            } else {
                attributeMacroCall(home, call, *macro->second.attributeMacro, out);
            }
        } else if (name == "store_accessor") {
            storeAccessor(home, call, out);
        } else if (name == "attribute") {
            attributeCall(home, call, out);
        } else if (name == "serialize") {
            serializeCall(home, call, out);
        } else if (name == "alias_attribute") {
            auto args = splitArguments(call);
            auto alias = args.positional.size() == 2 ? nameLiteral(args.positional[0]) : nullopt;
            auto old = args.positional.size() == 2 ? nameLiteral(args.positional[1]) : nullopt;
            if (!alias || !old) {
                problem(home, stat,
                        "can't read this `alias_attribute`: the plugin reads names that are symbols or strings");
                return;
            }
            change(home, call, out, AttributeChange::Kind::Alias, *alias, *old);
        } else if (auto association = associationMacro(name)) {
            associationCall(home, call, *association, out);
        } else if (name == "accepts_nested_attributes_for") {
            vector<string> names;
            for (auto arg : splitArguments(call).positional) {
                auto attribute = nameLiteral(arg);
                if (!attribute) {
                    names.clear();
                    break;
                }
                names.emplace_back(*attribute);
            }
            if (names.empty()) {
                problem(home, stat,
                        "can't read this `accepts_nested_attributes_for`: the plugin reads names that are symbols or "
                        "strings");
                return;
            }
            out.nestedAttributes.insert(out.nestedAttributes.end(), names.begin(), names.end());
        } else {
            nested(home, stat);
        }
    }

    void attachmentCall(const Definition &home, const pm_node_t *stat, const pm_call_node_t *attachment,
                        ClassMacros &out) {
        auto args = splitArguments(attachment);
        auto name = args.positional.size() == 1 && args.keywords.empty() && args.splats.empty() && args.readable &&
                            attachment->block == nullptr
                        ? nameLiteral(args.positional[0])
                        : nullopt;
        if (!name) {
            problem(home, stat,
                    "can't read this attachment: the plugin reads `Uploader.attachment(:name)` with a name that's a "
                    "symbol or string, and no options");
            return;
        }
        auto ref = constantRef(*home.file, attachment->receiver);
        auto uploader = ref ? resolve(app, home.nesting, *ref) : nullopt;
        if (!uploader) {
            problem(
                home, stat,
                fmt::format("can't type the attachment `{}`: its uploader isn't a class in the app's files", *name));
            return;
        }
        string error;
        auto plugins = uploaderPlugins(app, *uploader, error);
        if (!plugins) {
            problem(home, stat, fmt::format("can't type the attachment `{}`: {}", *name, error));
            return;
        }
        out.attachments.emplace_back(ShrineAttachmentInclude{*name, move(*plugins), level, home, stat});
    }

    void associationCall(const Definition &home, const pm_call_node_t *call, Association::Macro macro,
                         ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto args = splitArguments(call);
        auto name = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
        if (!name) {
            problem(home, node,
                    fmt::format("can't read this `{}`: the plugin reads an association whose name is a symbol or "
                                "string",
                                callName(*home.file, call)));
            return;
        }
        Association association{.macro = macro, .name = *name, .node = node};
        association.owner = owner->fullName;
        association.level = level;
        association.file = home.file;
        auto declaration = association.declaration();
        // The second positional argument is the scope (a lambda).
        if (args.positional.size() > 2 ||
            (args.positional.size() == 2 && PM_NODE_TYPE_P(args.positional[1], PM_HASH_NODE)) || !args.readable ||
            !args.splats.empty()) {
            problem(home, node,
                    fmt::format("can't read the options of `{}`: the plugin reads `key: value` options with symbol "
                                "keys",
                                declaration));
            return;
        }
        for (auto &[key, value] : args.keywords) {
            if (key == "class_name" || key == "through" || key == "source" || key == "source_type") {
                auto text = nameLiteral(value);
                if (!text) {
                    problem(
                        home, node,
                        fmt::format("can't read `{}:` of `{}`: the plugin reads a string or symbol", key, declaration));
                    return;
                }
                auto &option = key == "class_name" ? association.className
                               : key == "through"  ? association.through
                               : key == "source"   ? association.source
                                                   : association.sourceType;
                option = *text;
            } else if (key == "polymorphic") {
                auto kind = literal(value).kind;
                if (kind != Literal::Kind::True && kind != Literal::Kind::False && kind != Literal::Kind::Nil) {
                    problem(home, node,
                            fmt::format("can't read `polymorphic:` of `{}`: the plugin reads `true` or `false`",
                                        declaration));
                    return;
                }
                association.polymorphic = kind == Literal::Kind::True;
            } else if (key == "anonymous_class") {
                problem(home, node, fmt::format("doesn't type `anonymous_class:` (`{}`)", declaration));
                return;
            }
        }
        out.associations.emplace_back(move(association));
        out.associationSources.emplace_back(join(home.fullName));
    }

    // A macro call inside another statement (a block, an `if`), or called on a block's parameter (`with_options do
    // |assoc|`): Rails runs it, but the plugin doesn't read it.
    void nested(const Definition &home, const pm_node_t *stat) {
        auto isMacro = [&](string_view name) { return isModelMacro(name) || macros.contains(name); };
        for (auto call : macroCalls(*home.file, stat, isMacro, true)) {
            problem(home, reinterpret_cast<const pm_node_t *>(call),
                    fmt::format("can't read `{}` here: the plugin reads it only directly in a class body or a "
                                "concern's `included` block, with no receiver",
                                callName(*home.file, call)));
        }
    }

    // Macros a plain module's `def self.included(base)` runs on the class (`base.has_many`), which the plugin doesn't
    // read: a concern's `included do ... end` block is what it reads.
    void includedHooks(const Definition &def, const vector<Definition> &concerns) {
        auto isMacro = [&](string_view name) { return isModelMacro(name) || macros.contains(name); };
        set<string> seen;
        vector<const Definition *> includers{&def};
        for (auto &concern : concerns) {
            includers.emplace_back(&concern);
        }
        for (auto includer : includers) {
            for (auto &ref : includer->mixins("include")) {
                auto module = resolve(app, includer->nesting, ref);
                if (!module || module->isClass || !seen.insert(join(module->fullName)).second) {
                    continue;
                }
                auto hook = module->method("included", true);
                if (!hook || hook->def->body == nullptr) {
                    continue;
                }
                for (auto call : macroCalls(*module->file, hook->def->body, isMacro, true)) {
                    problem(*module, reinterpret_cast<const pm_node_t *>(call),
                            fmt::format("can't read `{}` in `self.included`: the plugin reads it only directly in a "
                                        "class body or a concern's `included do ... end` block",
                                        callName(*module->file, call)));
                }
            }
        }
    }

    void ignoredColumns(const Definition &home, const pm_node_t *stat, const pm_node_t *value, bool replace,
                        string_view written, ClassMacros &out) {
        IgnoredColumns ignored{replace, {}};
        auto readable = value != nullptr && PM_NODE_TYPE_P(value, PM_ARRAY_NODE);
        if (readable) {
            auto &elements = reinterpret_cast<const pm_array_node_t *>(value)->elements;
            for (size_t i = 0; i < elements.size && readable; i++) {
                auto name = nameLiteral(elements.nodes[i]);
                readable = name.has_value();
                if (name) {
                    ignored.names.emplace_back(*name);
                }
            }
        }
        if (!readable) {
            problem(home, stat,
                    fmt::format("can't read `self.ignored_columns {}`: the plugin reads an array of strings or symbols",
                                written));
            return;
        }
        out.ignoredColumns.emplace_back(move(ignored));
    }

    void attributeCall(const Definition &home, const pm_call_node_t *call, ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto args = splitArguments(call);
        auto name = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
        if (!name || args.positional.size() > 2 || !args.readable || !args.splats.empty()) {
            problem(home, node,
                    "can't read this `attribute`: the plugin reads a name that's a symbol or string, a type that's a "
                    "symbol, and options written out (`key: value`)");
            return;
        }
        if (args.positional.size() == 1) {
            change(home, call, out, AttributeChange::Kind::Declare, *name);
            return;
        }
        auto type = PM_NODE_TYPE_P(args.positional[1], PM_SYMBOL_NODE) ? nameLiteral(args.positional[1]) : nullopt;
        if (!type) {
            problem(home, node,
                    fmt::format("can't read the type of `attribute :{}`: the plugin reads a type that's a symbol "
                                "(`:boolean`)",
                                *name));
            return;
        }
        if (!knownAttributeType(*type)) {
            problem(home, node,
                    fmt::format("doesn't type `attribute :{}, :{}`: the plugin writes the types of the column types it "
                                "reads from db/structure.sql",
                                *name, *type));
            return;
        }
        bool array = false;
        for (auto &[key, value] : args.keywords) {
            if (key != "array" && key != "range") {
                continue;
            }
            auto option = literal(value);
            if (option.kind != Literal::Kind::True && option.kind != Literal::Kind::False &&
                option.kind != Literal::Kind::Nil) {
                problem(home, node,
                        fmt::format("can't read `{}:` of `attribute :{}`: the plugin reads `true` or "
                                    "`false`",
                                    key, *name));
                return;
            }
            if (key == "range" && option.kind == Literal::Kind::True) {
                problem(home, node, fmt::format("doesn't type `attribute :{}` with `range: true`", *name));
                return;
            }
            array = key == "array" ? option.kind == Literal::Kind::True : array;
        }
        change(home, call, out, AttributeChange::Kind::Type, *name, *type, array);
    }

    // `serialize :name, coder: ..., type: ...`. Without `coder:`, Rails uses `default_column_serializer`, which the
    // narrowed compiler in the app checks is YAML.
    void serializeCall(const Definition &home, const pm_call_node_t *call, ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto args = splitArguments(call);
        auto name = args.positional.size() == 1 ? nameLiteral(args.positional[0]) : nullopt;
        if (!name || !args.readable || !args.splats.empty()) {
            problem(home, node,
                    "can't read this `serialize`: the plugin reads a name that's a symbol or string, and options "
                    "written out (`key: value`)");
            return;
        }
        auto constant = [&](const pm_node_t *value) -> optional<string> {
            auto ref = constantRef(*home.file, value);
            return ref ? optional<string>(join(ref->name)) : nullopt;
        };
        bool yaml = true;
        string type = "Object";
        for (auto &[key, value] : args.keywords) {
            if (key == "coder") {
                auto coder = constant(value);
                if (coder == "YAML" || coder == "ActiveRecord::Coders::YAMLColumn") {
                    yaml = true;
                } else if (coder == "JSON" || coder == "ActiveRecord::Coders::JSON") {
                    yaml = false;
                } else {
                    problem(home, node,
                            fmt::format("can't read `coder:` of `serialize :{}`: the plugin reads `YAML` or `JSON`",
                                        *name));
                    return;
                }
            } else if (key == "type") {
                auto given = constant(value);
                if (!given) {
                    problem(
                        home, node,
                        fmt::format("can't read `type:` of `serialize :{}`: the plugin reads a class's name", *name));
                    return;
                }
                type = *given;
            }
        }
        change(home, call, out, AttributeChange::Kind::Declare, *name);
        change(home, call, out, AttributeChange::Kind::Serialize, *name,
               yaml && (type == "Array" || type == "Hash") ? type : "");
    }

    void attributeMacroCall(const Definition &home, const pm_call_node_t *call, const AttributeMacro &macro,
                            ClassMacros &out) {
        auto args = splitArguments(call);
        vector<string> names;
        for (auto arg : args.positional) {
            auto name = nameLiteral(arg);
            if (!name) {
                problem(home, reinterpret_cast<const pm_node_t *>(call),
                        fmt::format("can't read this `{}`: the plugin reads names that are symbols or strings",
                                    callName(*home.file, call)));
                return;
            }
            names.emplace_back(*name);
        }
        for (auto &name : names) {
            change(home, call, out, AttributeChange::Kind::Type, name, macro.type);
        }
    }

    void enumCall(const Definition &home, const pm_call_node_t *call, const EnumMacro *macro, ClassMacros &out) {
        auto node = reinterpret_cast<const pm_node_t *>(call);
        auto macroName = callName(*home.file, call);
        auto args = splitArguments(call);
        auto name = args.positional.empty() ? nullopt : nameLiteral(args.positional[0]);
        // With no values argument, `enum` takes every keyword as a value, and no options.
        const pm_node_t *valuesNode = args.positional.size() == 2 ? args.positional[1] : nullptr;
        if (macro == nullptr && args.positional.size() == 1 && !args.keywords.empty() && call->arguments != nullptr) {
            auto &arguments = call->arguments->arguments;
            auto last = arguments.nodes[arguments.size - 1];
            if (PM_NODE_TYPE_P(last, PM_KEYWORD_HASH_NODE)) {
                valuesNode = last;
                args.keywords.clear();
            }
        }
        if (!name || valuesNode == nullptr || !args.readable || !args.splats.empty()) {
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
        auto values = readValues(app, home, valuesNode, *name, 0, error);
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

        // The attribute: the macro's `attribute` call, then `enum`'s own (which keeps the type) and its decorator.
        auto typed = macro != nullptr && macro->attributeType;
        if (typed && macro->attributeFirst) {
            change(home, call, out, AttributeChange::Kind::Type, *name, *macro->attributeType);
        }
        change(home, call, out, AttributeChange::Kind::Enum, *name);
        if (typed && !macro->attributeFirst) {
            change(home, call, out, AttributeChange::Kind::Type, *name, *macro->attributeType);
        }
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

bool sameTypes(const Association &a, const Association &b) {
    return a.macro == b.macro && a.className == b.className && a.polymorphic == b.polymorphic &&
           a.through == b.through && a.source == b.source && a.sourceType == b.sourceType;
}

// The associations the classes declare, as Rails has them: a superclass's first, and each class's in order. The
// plugin reads a class's body before its concerns' `included` blocks, which Rails runs where the `include` is, so a
// name that both declare differently is an error.
ModelAssociations associationsOf(const Model &model, const vector<ClassMacros> &classes,
                                 vector<MacroProblem> &problems) {
    ModelAssociations out{model, vector<map<string, Association, less<>>>(classes.size()), {}};
    map<string, Association, less<>> current;
    set<string> seenNested;
    for (auto level = classes.size(); level-- > 0;) {
        auto &declared = classes[level];
        map<string, size_t, less<>> declaredAt;
        set<string, less<>> reported;
        for (size_t i = 0; i < declared.associations.size(); i++) {
            auto &association = declared.associations[i];
            auto [first, added] = declaredAt.emplace(association.name, i);
            if (!added) {
                auto &firstSource = declared.associationSources[first->second];
                if (firstSource != declared.associationSources[i] &&
                    !sameTypes(declared.associations[first->second], association) &&
                    reported.insert(association.name).second) {
                    problems.emplace_back(MacroProblem{
                        association.file, association.node,
                        fmt::format("can't type `{}`: `{}` declares it too, and the plugin doesn't know which runs "
                                    "first (a concern's `included` block runs at its `include`)",
                                    association.name, firstSource)});
                }
            }
            current.insert_or_assign(association.name, association);
        }
        for (auto &name : declared.nestedAttributes) {
            addUnique(out.nestedAttributes, seenNested, name);
        }
        out.levels[level] = current;
    }
    return out;
}

// What each class of the model's chain declares, the model first.
vector<ClassMacros> readClasses(const App &app, const Model &model, vector<MacroProblem> &problems) {
    // The app's own macros: the class methods of the model and its superclasses, and of the concerns they include
    // (`class_methods` blocks), that call model macros. The nearest definition of a name wins.
    map<string, AppMacro, less<>> macros;
    auto addMacros = [&](const RubyFile &file, const vector<const pm_node_t *> &stats, bool singleton) {
        for (auto stat : stats) {
            if (!PM_NODE_TYPE_P(stat, PM_DEF_NODE)) {
                continue;
            }
            auto method = reinterpret_cast<const pm_def_node_t *>(stat);
            auto name = string(file.name(method->name));
            auto calls = macroCalls(file, method->body, isModelMacro);
            if (singleton != isSelf(method->receiver) || calls.empty() || macros.contains(name)) {
                continue;
            }
            AppMacro macro;
            if (auto enumMacro = readEnumMacro(file, method, singleton)) {
                macro.enumMacro = move(*enumMacro);
            } else if (auto attributeMacro = readAttributeMacro(file, method, singleton)) {
                for (auto call : calls) {
                    if (call != attributeMacro->call) {
                        macro.problem = fmt::format("it calls `{}` too", callName(file, call));
                    }
                }
                macro.attributeMacro = *attributeMacro;
            } else {
                macro.problem = fmt::format("it calls `{}`, and the plugin reads only a method that passes its first "
                                            "two parameters to `enum`, or one that gives each name in its `*` "
                                            "parameter to `attribute` with a type",
                                            callName(file, calls.front()));
            }
            macros.emplace(name, move(macro));
        }
    };
    for (auto &def : model.chain) {
        addMacros(*def.file, def.body, true);
    }
    for (auto &def : model.chain) {
        for (auto &concern : includedConcerns(app, def)) {
            for (auto stat : concern.body) {
                auto call = asCall(stat);
                if (call != nullptr && call->receiver == nullptr && callName(*concern.file, call) == "class_methods" &&
                    call->block != nullptr && PM_NODE_TYPE_P(call->block, PM_BLOCK_NODE)) {
                    addMacros(*concern.file, statements(reinterpret_cast<const pm_block_node_t *>(call->block)->body),
                              false);
                }
            }
        }
    }

    Reader reader(app, macros, problems);
    vector<ClassMacros> classes;
    for (size_t level = 0; level < model.chain.size(); level++) {
        classes.emplace_back(reader.read(model.chain[level], level));
    }
    return classes;
}

} // namespace

ModelAssociations modelAssociations(const App &app, const Model &model, vector<MacroProblem> &problems) {
    return associationsOf(model, readClasses(app, model, problems), problems);
}

// The statement of the class's body that includes `concern`, itself or through the concerns it includes.
const pm_node_t *includeOf(const App &app, const Definition &def, const Definition &concern) {
    auto name = join(concern.fullName);
    for (auto stat : def.body) {
        auto call = asCall(stat);
        if (call == nullptr || call->receiver != nullptr || callName(*def.file, call) != "include") {
            continue;
        }
        for (auto arg : arguments(call)) {
            auto ref = constantRef(*def.file, arg);
            auto module = ref ? resolve(app, def.nesting, *ref) : nullopt;
            if (!module || module->isClass) {
                continue;
            }
            auto dependencies = includedConcerns(app, *module);
            if (join(module->fullName) == name ||
                absl::c_any_of(dependencies, [&](auto &dependency) { return join(dependency.fullName) == name; })) {
                return stat;
            }
        }
    }
    return nullptr;
}

string modelMacros(const App &app, const Model &model, const Inflector &inflector, const Table &table,
                   const optional<string> &primaryKey, vector<MacroProblem> &problems,
                   vector<AttachmentInclude> &attachmentIncludes) {
    auto classes = readClasses(app, model, problems);

    // Attributes: Rails applies a superclass's changes first, and each class's in order. The plugin reads a class's
    // body before its concerns' `included` blocks, which Rails runs where the `include` is, so an attribute that both
    // change is an error.
    vector<AttributeChange> changes;
    set<string, less<>> ignoredColumns;
    for (auto declared = classes.rbegin(); declared != classes.rend(); declared++) {
        map<string, string, less<>> changedIn;
        set<string, less<>> reported;
        for (size_t i = 0; i < declared->attributes.size(); i++) {
            auto &change = declared->attributes[i];
            auto &source = declared->attributeSources[i];
            if (change.kind != AttributeChange::Kind::Declare && change.kind != AttributeChange::Kind::Alias) {
                auto [first, added] = changedIn.emplace(change.name, source);
                if (!added && first->second != source && reported.insert(change.name).second) {
                    problems.emplace_back(MacroProblem{
                        change.file, change.node,
                        fmt::format("can't type `{}`: `{}` changes its type too, and the plugin doesn't know which "
                                    "runs first (a concern's `included` block runs at its `include`)",
                                    change.name, first->second)});
                }
            }
            changes.emplace_back(change);
        }
        for (auto &ignored : declared->ignoredColumns) {
            if (ignored.replace) {
                ignoredColumns.clear();
            }
            ignoredColumns.insert(ignored.names.begin(), ignored.names.end());
        }
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
        out += "end\n\n";
    }

    // Shrine attachments. Each one's methods go in a module of their own, included where Ruby includes the
    // attachment's module (see AttachmentInclude), so a module included after it overrides its methods as in Ruby.
    // A name declared more than once (again in an STI subclass, or twice in one class) is written once, from the
    // most specific declaration, which is the one Ruby finds. A method that the model, or a class between it and the
    // one that includes the attachment, defines itself is its own (ShrineAttachments wrote only those the
    // attachment's module defines for the model).
    vector<const ShrineAttachmentInclude *> chosen;
    set<string, less<>> attachmentNames;
    for (auto &declared : classes) {
        for (auto attachment = declared.attachments.rbegin(); attachment != declared.attachments.rend(); attachment++) {
            if (attachmentNames.insert(attachment->name).second) {
                chosen.emplace_back(&*attachment);
            }
        }
    }
    reverse(chosen.begin(), chosen.end());
    auto &self = model.chain.front();
    for (auto attachment : chosen) {
        auto methods = attachmentMethods(attachment->plugins, attachment->name, [&](string_view method) {
            for (size_t level = 0; level <= attachment->level && level < model.chain.size(); level++) {
                if (model.chain[level].method(method)) {
                    return true;
                }
            }
            return false;
        });
        if (methods.empty()) {
            continue;
        }
        AttachmentInclude placement{"GeneratedShrineAttachmentMethods_" + attachment->name};
        out += fmt::format("module {}\n{}end\n\n", placement.module, methods);
        if (attachment->level > 0) {
            placement.first = true;
        } else if (attachment->home.fullName == self.fullName && attachment->home.file->path == self.file->path) {
            placement.after = attachment->stat;
        } else {
            placement.after = includeOf(app, self, attachment->home);
        }
        attachmentIncludes.emplace_back(move(placement));
    }

    out += associationMethods(app, inflector, associationsOf(model, classes, problems), problems);
    out += attributeMethods(model, table, primaryKey, ignoredColumns, changes, problems);
    return out;
}

} // namespace sorbet::plugins::rails
