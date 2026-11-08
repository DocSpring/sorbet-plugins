// Associations: the methods Tapioca's ActiveRecordAssociations compiler wrote for a model's associations and nested
// attributes (ModelMacros.cc reads the calls), typed with the class Rails finds for each one.
//
// - `belongs_to` and `has_one`: the record (`T.nilable(<class>)`), its writer, `reload_` and `reset_`, and unless the
//   association is polymorphic, `build_`, `create_` and `create_!`. A `belongs_to` that the model itself declares (in
//   its body or a concern's `included` block) has `_changed?` and `_previously_changed?` too: Rails defines them in the
//   module of the class that declares it, and Tapioca writes the ones in the model's own module.
// - `has_many` and `has_and_belongs_to_many`: the class's PrivateCollectionProxy, its writer, and `<singular>_ids` and
//   its writer.
// - `accepts_nested_attributes_for :name`: `name_attributes=`.
//
// The class name is Rails' `class_name`: the `class_name:` option, or the association's name camelized (singular for a
// collection). A `through:` association's is its `class_name:`, its `source_type:` or its source's class name. Its
// source is the association of the through association's model that `source:` names, or else the one named with the
// singular or the plural of its name. Rails looks the class name up from the class that declares the association
// (`compute_type`): in that class's namespaces, innermost first (`Shop::Cart::Item`, `Shop::Item`), then at the top
// level (`Item`), and takes the first constant whose name is exactly that. A name that is the declaring class's own
// last part is looked for at the top level first (`_klass`), and one that starts with `::` only there. The plugin looks
// for each candidate in the app's files (Source.h's `resolve`), and writes a name it finds in none of them as given: a
// gem's model. A polymorphic association, and a through one whose source is polymorphic, is `T.untyped`, and its
// collection `ActiveRecord::Associations::CollectionProxy`.
//
// A constant the plugin can't see (a gem's, or one without its own file) that Rails would find ahead of the plugin's
// choice stops `bin/tapioca dsl`: the narrowed compiler in the app (sorbet/tapioca/compilers/gem_models.rb) checks
// each association our code declares against these rules.

#include "Inflector.h"
#include "Rails.h"

#include "absl/algorithm/container.h"

using namespace std;

namespace sorbet::plugins::rails {

string Association::declaration() const {
    string_view macroName;
    switch (macro) {
        case Macro::BelongsTo:
            macroName = "belongs_to";
            break;
        case Macro::HasOne:
            macroName = "has_one";
            break;
        case Macro::HasMany:
            macroName = "has_many";
            break;
        case Macro::HasAndBelongsToMany:
            macroName = "has_and_belongs_to_many";
            break;
    }
    return fmt::format("{} :{}", macroName, name);
}

namespace {

constexpr int MAX_THROUGH_DEPTH = 10;

ConstantName splitConstant(string_view name) {
    ConstantName out;
    size_t start = 0;
    while (true) {
        auto end = name.find("::", start);
        out.emplace_back(name.substr(start, end == string_view::npos ? string_view::npos : end - start));
        if (end == string_view::npos) {
            return out;
        }
        start = end + 2;
    }
}

bool isConstantName(const ConstantName &name) {
    return absl::c_all_of(name, [](const string &part) {
        return !part.empty() && isupper(static_cast<unsigned char>(part[0])) &&
               absl::c_all_of(part, [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    });
}

// Rails' `class_name` for an association, and whether it's polymorphic (a through one: whether its source is).
// `className` is nullopt for a polymorphic `belongs_to` without `class_name:`, which Rails never looks up.
struct ClassName {
    optional<string> className;
    bool polymorphic = false;
};

// The class a name finds: its definition in the app's files, or the name as given.
struct Target {
    string name; // the full name, without `::` in front
    optional<Definition> definition;
};

class Resolver {
public:
    Resolver(const App &app, const Inflector &inflector) : app(app), inflector(inflector) {}

    // The type an association's methods are written with: `::Category`, or `T.untyped`. nullopt with `error` set if
    // the plugin can't tell.
    optional<string> type(const ModelAssociations &in, const Association &association, string &error) {
        if (!association.through && association.polymorphic) {
            return "T.untyped";
        }
        auto name = className(in, association, error, 0);
        if (!name) {
            return nullopt;
        }
        auto target = find(association, *name->className, error);
        if (!target) {
            return nullopt;
        }
        return name->polymorphic ? "T.untyped" : "::" + target->name;
    }

private:
    const App &app;
    const Inflector &inflector;
    // The through models' associations, by name.
    map<string, ModelAssociations, less<>> loaded;

    optional<ClassName> className(const ModelAssociations &in, const Association &association, string &error,
                                  int depth) {
        if (!association.through) {
            if (association.polymorphic) {
                return ClassName{association.className, true};
            }
            if (association.className) {
                return ClassName{association.className, false};
            }
            auto name = association.collection() ? inflector.singularize(association.name) : association.name;
            return ClassName{inflector.camelize(name), false};
        }
        if (depth > MAX_THROUGH_DEPTH) {
            error = fmt::format("can't type `{}`: its `through:` associations go more than {} deep",
                                association.declaration(), MAX_THROUGH_DEPTH);
            return nullopt;
        }
        auto source = sourceOf(in, association, error, depth);
        if (!source) {
            return nullopt;
        }
        auto [throughIn, sourceAssociation] = *source;
        auto sourceName = className(*throughIn, *sourceAssociation, error, depth + 1);
        if (!sourceName) {
            return nullopt;
        }
        ClassName out{association.className, sourceName->polymorphic};
        if (!out.className) {
            out.className = association.sourceType ? association.sourceType : sourceName->className;
        }
        if (!out.className) {
            error = fmt::format("can't type `{}`: its source `{}#{}` is polymorphic, and Rails needs `source_type:` "
                                "for it",
                                association.declaration(), join(throughIn->model.chain.front().fullName),
                                sourceAssociation->name);
            return nullopt;
        }
        return out;
    }

    // A through association's source: the through model's associations, and the source among them.
    optional<pair<const ModelAssociations *, const Association *>>
    sourceOf(const ModelAssociations &in, const Association &association, string &error, int depth) {
        auto declaration = association.declaration();
        auto &visible = in.levels[association.level];
        auto through = visible.find(*association.through);
        if (through == visible.end()) {
            error = fmt::format("can't type `{}`: `{}` has no association `{}` (its `through:`)", declaration,
                                join(association.owner), *association.through);
            return nullopt;
        }
        auto throughName = className(in, through->second, error, depth + 1);
        if (!throughName) {
            return nullopt;
        }
        if (throughName->polymorphic) {
            error = fmt::format("can't type `{}`: its `through:` association `{}` is polymorphic", declaration,
                                through->first);
            return nullopt;
        }
        auto target = find(through->second, *throughName->className, error);
        if (!target) {
            return nullopt;
        }
        auto model = target->definition ? readModel(app, *target->definition) : nullopt;
        if (!model) {
            error = fmt::format("can't type `{}`: its `through:` association `{}` has the class `{}`, which isn't a "
                                "model in the app's files",
                                declaration, through->first, target->name);
            return nullopt;
        }
        auto &throughIn = load(*model);

        vector<string> names;
        if (association.source) {
            names.emplace_back(*association.source);
        } else {
            names.emplace_back(inflector.singularize(association.name));
            if (names.front() != association.name) {
                names.emplace_back(association.name);
            }
        }
        vector<string> found;
        for (auto &name : names) {
            if (throughIn.levels.front().contains(name)) {
                found.emplace_back(name);
            }
        }
        auto throughModel = join(model->chain.front().fullName);
        if (found.size() > 1) {
            error = fmt::format("can't type `{}`: `{}` has both `{}` and `{}`, so Rails can't tell which is the source "
                                "of `through: :{}`: give it `source:`",
                                declaration, throughModel, found[0], found[1], *association.through);
            return nullopt;
        }
        if (found.empty()) {
            error = fmt::format("can't type `{}`: `{}` has no association `{}` (the source of `through: :{}`)",
                                declaration, throughModel, fmt::join(names, "` or `"), *association.through);
            return nullopt;
        }
        return pair{&throughIn, &throughIn.levels.front().at(found.front())};
    }

    const ModelAssociations &load(const Model &model) {
        auto name = join(model.chain.front().fullName);
        auto found = loaded.find(name);
        if (found == loaded.end()) {
            // That model's own problems are reported where it's checked.
            vector<MacroProblem> ignored;
            found = loaded.emplace(name, modelAssociations(app, model, ignored)).first;
        }
        return found->second;
    }

    // The class Rails finds for a class name written in `association`'s declaring class.
    optional<Target> find(const Association &association, string_view className, string &error) {
        auto absolute = className.starts_with("::");
        auto name = splitConstant(absolute ? className.substr(2) : className);
        if (!isConstantName(name)) {
            error = fmt::format("can't type `{}`: `{}` isn't a constant name", association.declaration(), className);
            return nullopt;
        }
        vector<ConstantName> candidates;
        if (!absolute) {
            auto &owner = association.owner;
            if (name.size() == 1 && !owner.empty() && owner.back() == name.front()) {
                candidates.emplace_back(name);
            }
            for (auto size = owner.size(); size > 0; size--) {
                ConstantName candidate(owner.begin(), owner.begin() + size);
                candidate.insert(candidate.end(), name.begin(), name.end());
                candidates.emplace_back(move(candidate));
            }
        }
        candidates.emplace_back(name);
        for (auto &candidate : candidates) {
            auto definition = resolve(app, {}, ConstantRef{candidate, true});
            if (!definition) {
                continue;
            }
            if (!definition->isClass) {
                error = fmt::format("can't type `{}`: Rails finds the module `{}`, which isn't a model",
                                    association.declaration(), join(candidate));
                return nullopt;
            }
            return Target{join(candidate), move(definition)};
        }
        return Target{join(name), nullopt};
    }
};

} // namespace

string associationMethods(const App &app, const Inflector &inflector, const ModelAssociations &associations,
                          vector<MacroProblem> &problems) {
    auto &all = associations.levels.front();
    if (all.empty()) {
        return "";
    }
    Resolver resolver(app, inflector);
    string out = "include GeneratedAssociationMethods\n\nmodule GeneratedAssociationMethods\n";
    for (auto &name : associations.nestedAttributes) {
        out += fmt::format("  sig {{ params(attributes: T.untyped).returns(T.untyped) }}\n"
                           "  def {}_attributes=(attributes); end\n\n",
                           name);
    }
    for (auto &[name, association] : all) {
        string error;
        auto type = resolver.type(associations, association, error);
        if (!type) {
            problems.emplace_back(MacroProblem{association.file, association.node, move(error)});
            continue;
        }
        auto untyped = *type == "T.untyped";
        if (association.collection()) {
            auto proxy = untyped ? "ActiveRecord::Associations::CollectionProxy" : *type + "::PrivateCollectionProxy";
            out += fmt::format("  sig {{ returns({1}) }}\n  def {0}; end\n\n"
                               "  sig {{ params(value: T::Enumerable[{2}]).void }}\n  def {0}=(value); end\n\n"
                               "  sig {{ returns(T::Array[T.untyped]) }}\n  def {3}_ids; end\n\n"
                               "  sig {{ params(ids: T::Array[T.untyped]).returns(T::Array[T.untyped]) }}\n"
                               "  def {3}_ids=(ids); end\n\n",
                               name, proxy, *type, inflector.singularize(name));
            continue;
        }
        auto nilable = untyped ? *type : fmt::format("T.nilable({})", *type);
        out += fmt::format("  sig {{ returns({1}) }}\n  def {0}; end\n\n"
                           "  sig {{ params(value: {1}).void }}\n  def {0}=(value); end\n\n"
                           "  sig {{ returns({1}) }}\n  def reload_{0}; end\n\n"
                           "  sig {{ void }}\n  def reset_{0}; end\n\n",
                           name, nilable);
        if (association.macro == Association::Macro::BelongsTo && association.level == 0) {
            out += fmt::format("  sig {{ returns(T::Boolean) }}\n  def {0}_changed?; end\n\n"
                               "  sig {{ returns(T::Boolean) }}\n  def {0}_previously_changed?; end\n\n",
                               name);
        }
        if (!association.polymorphic) {
            out += fmt::format("  sig {{ params(args: T.untyped, blk: T.untyped).returns({1}) }}\n"
                               "  def build_{0}(*args, &blk); end\n\n"
                               "  sig {{ params(args: T.untyped, blk: T.untyped).returns({1}) }}\n"
                               "  def create_{0}(*args, &blk); end\n\n"
                               "  sig {{ params(args: T.untyped, blk: T.untyped).returns({1}) }}\n"
                               "  def create_{0}!(*args, &blk); end\n\n",
                               name, *type);
        }
    }
    out += "end\n\n";
    return out;
}

} // namespace sorbet::plugins::rails
