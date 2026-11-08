// Model columns: the methods Tapioca's ActiveRecordColumns compiler wrote in a model's GeneratedAttributeMethods, for
// each attribute: the reader, writer and query method, and the dirty-tracking ones (`_was`, `_changed?`, ...), 19 in
// all, with the types its column type helper gives them.
//
// The attributes are the columns of the model's table in db/structure.sql (without `ignored_columns`), `id`, the ones
// `attribute` declares, and the aliases: Rails' own `id_value`, and `alias_attribute`'s. A column's type comes from its
// SQL type, and the macros change it in the order they run (AttributeChange):
//
// - `attribute :name, :type` replaces it (`:decimal` over a `numeric(40,0)` column makes it BigDecimal, not Integer);
//   without a type it stays.
// - `enum` makes the reader a String and the writer take a String or Symbol, and an Integer if the type was an integer.
// - `serialize` with YAML (Rails' default) of an Array or a Hash makes it that class, never nil; any other is untyped.
//
// Nullability is Tapioca's "persisted" one: a column without NOT NULL makes the reader and writer nilable (a
// serialized Array or Hash reader stays as it is), and an attribute without a column isn't nilable.

#include "Rails.h"
#include "Schema.h"

using namespace std;

namespace sorbet::plugins::rails {

namespace {

// The classes Rails' attribute types cast to, as Tapioca names them. Value is ActiveModel::Type::Value, the type of an
// attribute declared without one; Json is a json or jsonb column's (both untyped, but an enum can't use Value).
enum class Base { Value, Json, Integer, String, Boolean, Date, Time, Decimal, Float, IPAddr, Duration };

struct Type {
    enum class Wrap { Enum, SerializedArray, SerializedHash, Serialized };
    Base base = Base::Value;
    bool array = false;
    vector<Wrap> wraps; // the decorators around the base type, innermost first
};

bool startsWith(string_view text, string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// The type Rails gives a column of this SQL type, as pg_dump writes it.
optional<Base> sqlBase(string_view sql) {
    if (sql == "bigint" || sql == "integer" || sql == "smallint") {
        return Base::Integer;
    }
    if (sql == "text" || sql == "uuid" || sql == "bytea" || sql == "character varying" ||
        (startsWith(sql, "character varying(") && endsWith(sql, ")"))) {
        return Base::String;
    }
    if (sql == "boolean") {
        return Base::Boolean;
    }
    if (sql == "date") {
        return Base::Date;
    }
    // Rails converts timestamps to the app's time zone (`time_zone_aware_attributes`).
    if (sql == "timestamp without time zone" ||
        (startsWith(sql, "timestamp(") && endsWith(sql, ") without time zone"))) {
        return Base::Time;
    }
    if (sql == "double precision" || sql == "real") {
        return Base::Float;
    }
    // `numeric(40,0)` has no scale: Rails reads it as an Integer (DecimalWithoutScale).
    if (sql == "numeric") {
        return Base::Decimal;
    }
    if (startsWith(sql, "numeric(") && endsWith(sql, ")")) {
        auto comma = sql.find(',');
        return comma == string_view::npos || sql.substr(comma) == ",0)" ? Base::Integer : Base::Decimal;
    }
    if (sql == "inet" || sql == "cidr") {
        return Base::IPAddr;
    }
    if (sql == "interval") {
        return Base::Duration;
    }
    if (sql == "json" || sql == "jsonb") {
        return Base::Json;
    }
    return nullopt;
}

optional<Base> symbolBase(string_view symbol) {
    if (symbol == "integer" || symbol == "big_integer") {
        return Base::Integer;
    }
    if (symbol == "string" || symbol == "text" || symbol == "uuid" || symbol == "binary") {
        return Base::String;
    }
    if (symbol == "boolean") {
        return Base::Boolean;
    }
    if (symbol == "date") {
        return Base::Date;
    }
    if (symbol == "datetime" || symbol == "time") {
        return Base::Time;
    }
    if (symbol == "decimal") {
        return Base::Decimal;
    }
    if (symbol == "float") {
        return Base::Float;
    }
    if (symbol == "inet" || symbol == "cidr") {
        return Base::IPAddr;
    }
    if (symbol == "interval") {
        return Base::Duration;
    }
    if (symbol == "json" || symbol == "jsonb") {
        return Base::Json;
    }
    return nullopt;
}

string baseName(Base base) {
    switch (base) {
        case Base::Value:
        case Base::Json:
            return "T.untyped";
        case Base::Integer:
            return "::Integer";
        case Base::String:
            return "::String";
        case Base::Boolean:
            return "T::Boolean";
        case Base::Date:
            return "::Date";
        case Base::Time:
            return "::ActiveSupport::TimeWithZone";
        case Base::Decimal:
            return "::BigDecimal";
        case Base::Float:
            return "::Float";
        case Base::IPAddr:
            return "::IPAddr";
        case Base::Duration:
            return "::ActiveSupport::Duration";
    }
}

// The reader's type, before nullability: the outermost decorator decides.
string readerType(const Type &type) {
    if (!type.wraps.empty()) {
        switch (type.wraps.back()) {
            case Type::Wrap::Enum:
                return "::String";
            case Type::Wrap::SerializedArray:
                return "T::Array[T.untyped]";
            case Type::Wrap::SerializedHash:
                return "T::Hash[T.untyped, T.untyped]";
            case Type::Wrap::Serialized:
                return "T.untyped";
        }
    }
    auto name = baseName(type.base);
    return type.array ? fmt::format("T::Array[{}]", name) : name;
}

string nilable(const string &type) {
    if (type == "T.untyped" || startsWith(type, "T.nilable(")) {
        return type;
    }
    return fmt::format("T.nilable({})", type);
}

bool isAttributeName(string_view name) {
    if (name.empty() || isdigit(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    for (char c : name) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

struct Attribute {
    string name;
    Type type;
};

// The 19 methods of one attribute, as Tapioca writes them.
void writeMethods(string &out, const string &name, const string &reader, const string &writer) {
    auto change = fmt::format("T.nilable([{0}, {0}])", reader);
    auto was = nilable(reader);
    auto fromTo = "sig { params(from: T.untyped, to: T.untyped).returns(T::Boolean) }\n";
    auto fromToParams = "(from: T.unsafe(nil), to: T.unsafe(nil))";
    out += fmt::format("  sig {{ returns({1}) }}\n  def {0}; end\n\n"
                       "  sig {{ params(value: {2}).returns({2}) }}\n  def {0}=(value); end\n\n"
                       "  sig {{ returns(T::Boolean) }}\n  def {0}?; end\n\n"
                       "  sig {{ returns({3}) }}\n  def {0}_before_last_save; end\n\n"
                       "  sig {{ returns({4}) }}\n  def {0}_change_to_be_saved; end\n\n"
                       "  sig {{ returns({3}) }}\n  def {0}_in_database; end\n\n"
                       "  sig {{ returns({4}) }}\n  def saved_change_to_{0}; end\n\n"
                       "  {5}  def saved_change_to_{0}?{6}; end\n\n"
                       "  {5}  def will_save_change_to_{0}?{6}; end\n\n"
                       "  sig {{ returns({4}) }}\n  def {0}_change; end\n\n"
                       "  {5}  def {0}_changed?{6}; end\n\n"
                       "  sig {{ void }}\n  def {0}_will_change!; end\n\n"
                       "  sig {{ returns({3}) }}\n  def {0}_was; end\n\n"
                       "  sig {{ returns({4}) }}\n  def {0}_previous_change; end\n\n"
                       "  {5}  def {0}_previously_changed?{6}; end\n\n"
                       "  sig {{ returns({3}) }}\n  def {0}_previously_was; end\n\n"
                       "  sig {{ void }}\n  def restore_{0}!; end\n\n"
                       "  sig {{ returns(T.untyped) }}\n  def {0}_before_type_cast; end\n\n"
                       "  sig {{ returns(T::Boolean) }}\n  def {0}_came_from_user?; end\n\n",
                       name, reader, writer, was, change, fromTo, fromToParams);
}

} // namespace

bool knownAttributeType(string_view symbol) {
    return symbolBase(symbol).has_value();
}

string attributeMethods(const Model &model, const Table &table, const optional<string> &primaryKey,
                        const set<string, less<>> &ignoredColumns, const vector<AttributeChange> &changes,
                        vector<MacroProblem> &problems) {
    auto &def = model.chain.front();
    auto classProblem = [&](string message) { problems.emplace_back(MacroProblem{def.file, def.node, move(message)}); };

    // Rails' attribute set: the columns in the table's order, then the attributes the macros add.
    vector<Attribute> attributes;
    auto find = [&](string_view name) -> Attribute * {
        for (auto &attribute : attributes) {
            if (attribute.name == name) {
                return &attribute;
            }
        }
        return nullptr;
    };
    auto column = [&](string_view name) -> const Column * {
        return ignoredColumns.contains(name) ? nullptr : table.column(name);
    };
    for (auto &tableColumn : table.columns) {
        if (ignoredColumns.contains(tableColumn.name)) {
            continue;
        }
        auto sql = string_view(tableColumn.sqlType);
        Type type;
        if (endsWith(sql, "[]")) {
            type.array = true;
            sql.remove_suffix(2);
        }
        auto base = sqlBase(sql);
        if (!base || (type.array && *base == Base::Time)) {
            classProblem(fmt::format("doesn't type the column `{}` of type `{}` (db/structure.sql)", tableColumn.name,
                                     tableColumn.sqlType));
            continue;
        }
        type.base = *base;
        attributes.emplace_back(Attribute{tableColumn.name, move(type)});
    }

    vector<pair<string, string>> aliases;
    for (auto &change : changes) {
        auto problem = [&](string message) {
            problems.emplace_back(MacroProblem{change.file, change.node, move(message)});
        };
        auto declare = [&]() -> Attribute & {
            if (auto found = find(change.name)) {
                return *found;
            }
            return attributes.emplace_back(Attribute{change.name, Type{}});
        };
        switch (change.kind) {
            case AttributeChange::Kind::Declare:
                declare();
                break;
            case AttributeChange::Kind::Type: {
                auto base = symbolBase(change.detail);
                if (!base || (change.array && *base == Base::Time)) {
                    problem(fmt::format("doesn't type `attribute :{}, :{}`{}", change.name, change.detail,
                                        change.array ? " with `array: true`" : ""));
                    break;
                }
                declare().type = Type{*base, change.array, {}};
                break;
            }
            case AttributeChange::Kind::Enum: {
                auto &attribute = declare();
                auto &type = attribute.type;
                if (type.base == Base::Value && !type.array && type.wraps.empty()) {
                    problem(fmt::format("`enum :{0}` has no column or type: Rails raises \"Undeclared attribute type "
                                        "for enum\" (add a column, or `attribute :{0}, :integer` before it)",
                                        change.name));
                    break;
                }
                type.wraps.emplace_back(Type::Wrap::Enum);
                break;
            }
            case AttributeChange::Kind::Serialize:
                declare().type.wraps.emplace_back(change.detail == "Array"  ? Type::Wrap::SerializedArray
                                                  : change.detail == "Hash" ? Type::Wrap::SerializedHash
                                                                            : Type::Wrap::Serialized);
                break;
            case AttributeChange::Kind::Alias:
                aliases.emplace_back(change.name, change.detail);
                break;
        }
    }

    // Tapioca's types for an attribute (`column_type_for`): the reader's and the writer's.
    auto typesOf = [&](string_view name) -> pair<string, string> {
        auto attribute = find(name);
        auto type = attribute != nullptr ? attribute->type : Type{};
        auto reader = readerType(type);
        auto writer = reader;
        if (!type.wraps.empty() && type.wraps.back() == Type::Wrap::Enum) {
            auto integer = type.wraps.size() == 1 && type.base == Base::Integer && !type.array;
            writer = integer ? "T.any(::String, ::Symbol, ::Integer)" : "T.any(::String, ::Symbol)";
        }
        auto tableColumn = column(name);
        if (tableColumn == nullptr || tableColumn->notNull) {
            return {reader, writer};
        }
        auto serialized = !type.wraps.empty() && (type.wraps.back() == Type::Wrap::SerializedArray ||
                                                  type.wraps.back() == Type::Wrap::SerializedHash);
        return {serialized ? reader : nilable(reader), nilable(writer)};
    };
    // `id` has the primary key's types, whatever its column is called; untyped without one.
    auto idTypes = [&]() -> pair<string, string> {
        return primaryKey ? typesOf(*primaryKey) : pair<string, string>{"T.untyped", "T.untyped"};
    };

    string body;
    auto write = [&](const string &name, pair<string, string> types) {
        if (!isAttributeName(name)) {
            classProblem(fmt::format("can't write the methods of the attribute `{}`: it isn't a method name", name));
            return;
        }
        writeMethods(body, name, types.first, types.second);
    };
    for (auto &attribute : attributes) {
        write(attribute.name, attribute.name == "id" ? idTypes() : typesOf(attribute.name));
    }
    if (find("id") == nullptr) {
        write("id", idTypes());
    }
    // Rails aliases `id_value` to `id` when the model has an `id` attribute and no `id_value`.
    if (find("id") != nullptr && find("id_value") == nullptr) {
        write("id_value", typesOf("id"));
    }
    for (auto &[name, old] : aliases) {
        write(name, name == "id" ? idTypes() : typesOf(old));
    }
    return fmt::format("include GeneratedAttributeMethods\n\nmodule GeneratedAttributeMethods\n{}end\n", body);
}

} // namespace sorbet::plugins::rails
