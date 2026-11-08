// Models: the methods Tapioca's ActiveRecordRelations compiler wrote for every model (`where`, `find`, the relation
// classes), from the relations template (templates/relations.rbi) with the model's name and primary key type, the
// methods of the scopes, enums and store accessors in its code (ModelMacros.cc), its associations' methods
// (ModelAssociations.cc), and its attributes' methods (ModelColumns.cc).
//
// A model is a class in an app file whose superclass chain reaches ActiveRecord::Base through the app's own classes
// (ApplicationRecord, an STI parent), and isn't abstract. A class with a gem's superclass (`< Audited::Audit`) isn't
// one: Tapioca keeps writing its methods. The narrowed compiler in the app (sorbet/tapioca/compilers/) uses the same
// rule.

#include "Inflector.h"
#include "Rails.h"
#include "Schema.h"
#include "Template.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

constexpr int MAX_MODEL_DEPTH = 10;

// `self.abstract_class = true` or `primary_abstract_class` in the body.
bool isAbstract(const Definition &def) {
    for (auto stat : def.body) {
        auto call = asCall(stat);
        if (call == nullptr) {
            continue;
        }
        auto name = callName(*def.file, call);
        auto args = arguments(call);
        if (name == "primary_abstract_class" && call->receiver == nullptr && args.empty()) {
            return true;
        }
        if (name == "abstract_class=" && isSelf(call->receiver) && args.size() == 1 &&
            PM_NODE_TYPE_P(args[0], PM_TRUE_NODE)) {
            return true;
        }
    }
    return false;
}

// The value of `self.<attribute> = 'literal'` in the body (a string or symbol). `error` is set if the body sets it
// to something else.
optional<string> literalSetting(const Definition &def, string_view attribute, string &error) {
    auto setter = string(attribute) + "=";
    optional<string> value;
    for (auto stat : def.body) {
        auto call = asCall(stat);
        if (call == nullptr || !isSelf(call->receiver) || callName(*def.file, call) != setter) {
            continue;
        }
        auto args = arguments(call);
        if (args.size() == 1 && PM_NODE_TYPE_P(args[0], PM_STRING_NODE)) {
            value = stringValue(reinterpret_cast<const pm_string_node_t *>(args[0])->unescaped);
        } else if (args.size() == 1 && PM_NODE_TYPE_P(args[0], PM_SYMBOL_NODE)) {
            value = stringValue(reinterpret_cast<const pm_symbol_node_t *>(args[0])->unescaped);
        } else {
            error = fmt::format("can't read `self.{} =` (line {}): set it to a literal string", attribute,
                                def.file->line(stat));
            return nullopt;
        }
    }
    return value;
}

} // namespace

optional<vector<Definition>> recordChain(const App &app, const Definition &def) {
    vector<Definition> chain{def};
    for (int depth = 0; depth < MAX_MODEL_DEPTH; depth++) {
        auto &current = chain.back();
        if (!current.isClass || current.superclass == nullptr) {
            return nullopt;
        }
        auto superclass = constantRef(*current.file, current.superclass);
        if (!superclass) {
            return nullopt;
        }
        if (join(superclass->name) == "ActiveRecord::Base") {
            return chain;
        }
        auto parent = resolve(app, current.outerNesting(), *superclass);
        if (!parent || !parent->isClass) {
            return nullopt;
        }
        chain.emplace_back(move(*parent));
    }
    return nullopt;
}

optional<Model> readModel(const App &app, const Definition &def) {
    auto chain = recordChain(app, def);
    if (!chain || isAbstract(chain->front())) {
        return nullopt;
    }
    Model model{*chain, 0};
    // Rails' base_class: up the chain while the superclass is a model of its own (STI).
    while (model.base + 1 < chain->size() && !isAbstract((*chain)[model.base + 1])) {
        model.base++;
    }
    return model;
}

// The model's table, as Rails' `compute_table_name` names it, or an error message.
static optional<string> tableName(const Model &model, const Inflector &inflector, string &error) {
    // A table name set on the class or (for an STI subclass) on the classes between it and its base class.
    for (size_t i = 0; i <= model.base; i++) {
        auto set = literalSetting(model.chain[i], "table_name", error);
        if (!error.empty()) {
            return nullopt;
        }
        if (set) {
            return set;
        }
    }
    auto &base = model.chain[model.base];
    if (base.fullName.size() > 1) {
        error = fmt::format("`{}` needs `self.table_name = '...'`: the plugin doesn't work out the table name of a "
                            "model in a namespace (a module's `table_name_prefix`, a parent model's table)",
                            join(base.fullName));
        return nullopt;
    }
    return inflector.pluralize(inflector.underscore(base.fullName.back()));
}

// The primary key column: `self.primary_key =` in the class or a superclass, or the table's. nullopt for a table
// without one; `error` is set for a composite key or a column the table doesn't have.
static optional<string> primaryKeyColumn(const Model &model, const Table &table, string &error) {
    optional<string> primaryKey;
    for (size_t i = 0; i < model.chain.size() && !primaryKey; i++) {
        primaryKey = literalSetting(model.chain[i], "primary_key", error);
        if (!error.empty()) {
            return nullopt;
        }
    }
    vector<string> columns = primaryKey ? vector<string>{*primaryKey} : table.primaryKey;
    if (columns.empty()) {
        return nullopt;
    }
    if (columns.size() > 1) {
        error = "doesn't type models with a composite primary key";
        return nullopt;
    }
    if (table.column(columns[0]) == nullptr) {
        error = fmt::format("can't find the primary key column `{}` in db/structure.sql", columns[0]);
        return nullopt;
    }
    return columns[0];
}

// `T::Array[...]`'s element type for `ids`: the primary key's type as Tapioca writes it, or an error message.
static optional<string> idType(const Table &table, const optional<string> &primaryKey, string &error) {
    if (!primaryKey) {
        return "T.untyped";
    }
    auto &type = table.column(*primaryKey)->sqlType;
    if (type == "bigint" || type == "integer" || type == "smallint") {
        return "::Integer";
    }
    if (type == "uuid" || type == "text" ||
        type.substr(0, string_view("character varying").size()) == "character varying") {
        if (type.find("[]") == string::npos) {
            return "::String";
        }
    }
    error = fmt::format("doesn't type a primary key of type `{}`", type);
    return nullopt;
}

void rewriteModels(core::MutableContext ctx, ast::ClassDef &root, const App &app, const shared_ptr<RubyFile> &file) {
    optional<Schema> schema;
    optional<Inflector> inflector;
    string inflectorError;
    for (auto &def : definitions(file)) {
        if (!def.isClass || def.superclass == nullptr) {
            continue;
        }
        // Only in the file Zeitwerk loads the class from: a file that reopens it adds nothing.
        auto main = resolve(app, {}, ConstantRef{def.fullName, true});
        if (!main || main->file->path != file->path) {
            continue;
        }
        auto model = readModel(app, def);
        if (!model) {
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
        if (!schema) {
            schema = readSchema(app);
            if (!schema) {
                reportError(ctx, loc, "can't read the app's tables from db/structure.sql");
                return;
            }
        }
        string error;
        auto table = tableName(*model, *inflector, error);
        if (!table) {
            reportError(ctx, loc, error);
            continue;
        }
        auto found = schema->find(*table);
        if (found == schema->end()) {
            reportError(ctx, loc, fmt::format("can't find the table `{}` in db/structure.sql", *table));
            continue;
        }
        auto primaryKey = primaryKeyColumn(*model, found->second, error);
        auto ids = error.empty() ? idType(found->second, primaryKey, error) : nullopt;
        if (!ids) {
            reportError(ctx, loc, error);
            continue;
        }
        auto source = replaceAll(string(templates::relations), "__MODEL__", join(def.fullName));
        source = replaceAll(move(source), "__IDS__", *ids);
        addTemplate(ctx, loc, tree->rhs, move(source), "relations");

        vector<MacroProblem> problems;
        auto macros = modelMacros(app, *model, *inflector, found->second, primaryKey, problems);
        for (auto &problem : problems) {
            if (problem.file->path == file->path) {
                auto [begin, end] = file->offsets(problem.node);
                reportError(ctx, core::LocOffsets{begin, end}, problem.message);
            } else {
                auto path = app.relative(problem.file->path).value_or(problem.file->path);
                reportError(ctx, loc,
                            fmt::format("{} line {}: {}", path, problem.file->line(problem.node), problem.message));
            }
        }
        if (!macros.empty()) {
            addTemplate(ctx, loc, tree->rhs, move(macros), "model macros");
        }
    }
}

} // namespace sorbet::plugins::rails
