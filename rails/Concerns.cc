// Concerns: the methods `class_attribute` in a concern's `included` block defines on the classes that include it, and
// the class methods a concern brings from the concerns it includes. These are what Tapioca's MixedInClassAttributes
// and ActiveSupportConcern compilers wrote for the app's concerns.
//
// `class_attribute :name` in an `included` block becomes `GeneratedClassMethods` (the class reader, writer and
// predicate) and `GeneratedInstanceMethods` (the instance ones its options leave), which the concern includes and
// mixes into the including class's class methods. Rails' rules: the class always gets `name` and `name=`, and `name?`
// unless `instance_predicate: false`; an instance gets `name` unless `instance_reader` (or `instance_accessor`) is
// false, `name=` unless `instance_writer` (or `instance_accessor`) is false, and `name?` when it has the reader and
// the predicate. The values are `T.untyped`, as Tapioca wrote them.
//
// ActiveSupport::Concern includes a concern's own concerns in the class that includes it, so that class gets their
// class methods too. Sorbet applies `mixes_in_class_methods` only for the module a class includes itself, so each
// concern mixes in the `ClassMethods` and `GeneratedClassMethods` of every app concern it includes, however deep.

#include "Rails.h"

#include <set>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_CONCERN_DEPTH = 10;

struct ClassAttribute {
    string name;
    bool instanceReader;
    bool instanceWriter;
    bool instancePredicate;
};

// The `class_attribute` calls among the direct statements of a concern's `included` blocks.
vector<const pm_call_node_t *> classAttributeCalls(const Definition &def) {
    vector<const pm_call_node_t *> out;
    for (auto block : def.includedBlocks()) {
        for (auto stat : statements(block->body)) {
            if (!PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
                continue;
            }
            auto call = reinterpret_cast<const pm_call_node_t *>(stat);
            if (call->receiver == nullptr && callName(*def.file, call) == "class_attribute") {
                out.emplace_back(call);
            }
        }
    }
    return out;
}

optional<bool> booleanLiteral(const pm_node_t *node) {
    if (PM_NODE_TYPE_P(node, PM_TRUE_NODE)) {
        return true;
    }
    if (PM_NODE_TYPE_P(node, PM_FALSE_NODE)) {
        return false;
    }
    return nullopt;
}

// The attributes one `class_attribute` call defines, or nullopt if it has a name that isn't a symbol or string, an
// option that isn't `true` or `false` (`default:` can be anything), or an option Rails doesn't take.
optional<vector<ClassAttribute>> readClassAttribute(const RubyFile &file, const pm_call_node_t *call) {
    vector<string> names;
    optional<bool> accessor, reader, writer, predicate;
    for (auto arg : arguments(call)) {
        if (PM_NODE_TYPE_P(arg, PM_SYMBOL_NODE)) {
            names.emplace_back(stringValue(reinterpret_cast<const pm_symbol_node_t *>(arg)->unescaped));
            continue;
        }
        if (PM_NODE_TYPE_P(arg, PM_STRING_NODE)) {
            names.emplace_back(stringValue(reinterpret_cast<const pm_string_node_t *>(arg)->unescaped));
            continue;
        }
        if (!PM_NODE_TYPE_P(arg, PM_KEYWORD_HASH_NODE)) {
            return nullopt;
        }
        auto &elements = reinterpret_cast<const pm_keyword_hash_node_t *>(arg)->elements;
        for (size_t i = 0; i < elements.size; i++) {
            auto element = elements.nodes[i];
            if (!PM_NODE_TYPE_P(element, PM_ASSOC_NODE)) {
                return nullopt;
            }
            auto assoc = reinterpret_cast<const pm_assoc_node_t *>(element);
            if (!PM_NODE_TYPE_P(assoc->key, PM_SYMBOL_NODE)) {
                return nullopt;
            }
            auto key = stringValue(reinterpret_cast<const pm_symbol_node_t *>(assoc->key)->unescaped);
            if (key == "default") {
                continue;
            }
            auto value = booleanLiteral(assoc->value);
            if (!value) {
                return nullopt;
            }
            if (key == "instance_accessor") {
                accessor = value;
            } else if (key == "instance_reader") {
                reader = value;
            } else if (key == "instance_writer") {
                writer = value;
            } else if (key == "instance_predicate") {
                predicate = value;
            } else {
                return nullopt;
            }
        }
    }
    auto instanceAccessor = accessor.value_or(true);
    vector<ClassAttribute> out;
    for (auto &name : names) {
        out.emplace_back(ClassAttribute{name, reader.value_or(instanceAccessor), writer.value_or(instanceAccessor),
                                        predicate.value_or(true)});
    }
    return out;
}

// Whether a concern defines class methods for the classes that include it: a `class_methods` block or its own
// `module ClassMethods`.
bool hasClassMethods(const Definition &def) {
    for (auto stat : def.body) {
        if (PM_NODE_TYPE_P(stat, PM_CALL_NODE)) {
            auto call = reinterpret_cast<const pm_call_node_t *>(stat);
            if (call->receiver == nullptr && callName(*def.file, call) == "class_methods" && call->block != nullptr) {
                return true;
            }
        } else if (PM_NODE_TYPE_P(stat, PM_MODULE_NODE)) {
            auto ref = constantRef(*def.file, reinterpret_cast<const pm_module_node_t *>(stat)->constant_path);
            if (ref && !ref->absolute && ref->name == ConstantName{"ClassMethods"}) {
                return true;
            }
        }
    }
    return false;
}

// The app concerns `def` includes, each one's own before it, once each: the order ActiveSupport::Concern includes
// them in the class that includes `def`.
void concernDependencies(const App &app, const Definition &def, int depth, set<string> &seen, vector<Definition> &out) {
    if (depth > MAX_CONCERN_DEPTH) {
        return;
    }
    for (auto &ref : def.mixins("include")) {
        auto found = resolve(app, def.nesting, ref);
        if (!found || found->isClass || !found->isConcern() || !seen.insert(join(found->fullName)).second) {
            continue;
        }
        concernDependencies(app, *found, depth + 1, seen, out);
        out.emplace_back(*found);
    }
}

ast::ExpressionPtr generatedModule(core::MutableContext ctx, core::LocOffsets loc, string_view name,
                                   ast::ClassDef::RHS_store body) {
    return ast::MK::Module(loc, loc, constantTree(ctx, loc, ConstantRef{{string(name)}, false}), move(body));
}

void addAccessor(core::MutableContext ctx, ast::ClassDef::RHS_store &body, core::LocOffsets loc, const string &name,
                 bool writer) {
    ast::MethodDef::PARAMS_store params;
    ast::Send::ARGS_store sigArgs;
    if (writer) {
        auto value = ctx.state.enterNameUTF8("value");
        params.emplace_back(ast::MK::Local(loc, value));
        sigArgs.emplace_back(ast::MK::Symbol(loc, value));
        sigArgs.emplace_back(ast::MK::Untyped(loc));
    }
    addMethod(ctx, body, loc, writer ? name + "=" : name, move(params), move(sigArgs), ast::MK::Untyped(loc), false);
}

// `mixes_in_class_methods`, as Sorbet's own rewrite of a `class_methods` block writes it, so the concern needn't
// extend T::Helpers.
ast::ExpressionPtr mixesInClassMethods(core::MutableContext ctx, core::LocOffsets loc, const ConstantName &module) {
    return ast::MK::Send2(loc, ast::MK::Magic(loc), core::Names::mixesInClassMethods(), loc, ast::MK::Self(loc),
                          constantTree(ctx, loc, ConstantRef{module, true}));
}

} // namespace

void rewriteConcerns(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    for (auto &def : definitions(file)) {
        if (def.isClass || !def.isConcern()) {
            continue;
        }
        auto tree = findTreeClass(ctx, root, def.fullName);
        if (tree == nullptr) {
            continue;
        }
        auto loc = zeroAt(*tree);

        auto calls = classAttributeCalls(def);
        if (!calls.empty()) {
            ast::ClassDef::RHS_store classMethods, instanceMethods;
            for (auto call : calls) {
                auto attributes = readClassAttribute(*file, call);
                if (!attributes) {
                    auto [begin, end] = file->offsets(reinterpret_cast<const pm_node_t *>(call));
                    reportError(ctx, core::LocOffsets{begin, end},
                                "can't read this `class_attribute`: the plugin writes its methods from names that are "
                                "symbols or strings, and `instance_accessor`, `instance_reader`, `instance_writer` and "
                                "`instance_predicate` options that are `true` or `false`");
                    continue;
                }
                for (auto &attribute : *attributes) {
                    addAccessor(ctx, classMethods, loc, attribute.name, false);
                    addAccessor(ctx, classMethods, loc, attribute.name, true);
                    if (attribute.instancePredicate) {
                        addAccessor(ctx, classMethods, loc, attribute.name + "?", false);
                    }
                    if (attribute.instanceReader) {
                        addAccessor(ctx, instanceMethods, loc, attribute.name, false);
                        if (attribute.instancePredicate) {
                            addAccessor(ctx, instanceMethods, loc, attribute.name + "?", false);
                        }
                    }
                    if (attribute.instanceWriter) {
                        addAccessor(ctx, instanceMethods, loc, attribute.name, true);
                    }
                }
            }
            auto classModule = def.fullName;
            classModule.emplace_back("GeneratedClassMethods");
            auto instanceModule = def.fullName;
            instanceModule.emplace_back("GeneratedInstanceMethods");
            tree->rhs.emplace_back(generatedModule(ctx, loc, "GeneratedClassMethods", move(classMethods)));
            tree->rhs.emplace_back(generatedModule(ctx, loc, "GeneratedInstanceMethods", move(instanceMethods)));
            tree->rhs.emplace_back(mixinStatement(ctx, loc, "include", instanceModule));
            tree->rhs.emplace_back(mixesInClassMethods(ctx, loc, classModule));
        }

        set<string> seen{join(def.fullName)};
        vector<Definition> dependencies;
        concernDependencies(app, def, 0, seen, dependencies);
        for (auto &dependency : dependencies) {
            if (hasClassMethods(dependency)) {
                auto module = dependency.fullName;
                module.emplace_back("ClassMethods");
                tree->rhs.emplace_back(mixesInClassMethods(ctx, loc, module));
            }
            if (!classAttributeCalls(dependency).empty()) {
                auto module = dependency.fullName;
                module.emplace_back("GeneratedClassMethods");
                tree->rhs.emplace_back(mixesInClassMethods(ctx, loc, module));
            }
        }
    }
}

} // namespace sorbet::plugins::rails
