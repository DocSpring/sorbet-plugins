#include "Schema.h"

#include <fstream>
#include <sstream>

using namespace std;

namespace sorbet::plugins::rails {

namespace {

bool startsWith(string_view text, string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

string_view trim(string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// A column line of a CREATE TABLE: `name type [COLLATE ...] [DEFAULT ...] [NOT NULL]` (a reserved word as the name is
// quoted), or nullopt for a constraint.
optional<Column> readColumn(string_view line) {
    line = trim(line);
    if (!line.empty() && line.back() == ',') {
        line.remove_suffix(1);
    }
    if (line.empty() || startsWith(line, "CONSTRAINT ")) {
        return nullopt;
    }
    Column column;
    size_t rest;
    if (line.front() == '"') {
        auto close = line.find('"', 1);
        if (close == string_view::npos) {
            return nullopt;
        }
        column.name = string(line.substr(1, close - 1));
        rest = close + 1;
    } else {
        auto space = line.find(' ');
        if (space == string_view::npos) {
            return nullopt;
        }
        column.name = string(line.substr(0, space));
        rest = space;
    }
    auto definition = trim(line.substr(rest));
    constexpr string_view notNull = " NOT NULL";
    if (definition.size() >= notNull.size() && definition.substr(definition.size() - notNull.size()) == notNull) {
        column.notNull = true;
    }
    auto end = definition.size();
    for (string_view keyword : {" COLLATE ", " DEFAULT ", " NOT NULL", " GENERATED "}) {
        auto at = definition.find(keyword);
        if (at != string_view::npos && at < end) {
            end = at;
        }
    }
    column.sqlType = string(definition.substr(0, end));
    return column;
}

} // namespace

const Column *Table::column(string_view name) const {
    for (auto &column : columns) {
        if (column.name == name) {
            return &column;
        }
    }
    return nullptr;
}

optional<Schema> readSchema(const App &app) {
    auto path = app.find("db/structure.sql");
    if (!path) {
        return nullopt;
    }
    ifstream in(*path, ios::binary);
    if (!in) {
        return nullopt;
    }
    Schema schema;
    string line;
    Table *current = nullptr;
    string altered;
    constexpr string_view createTable = "CREATE TABLE public.";
    constexpr string_view alterTable = "ALTER TABLE ONLY public.";
    while (getline(in, line)) {
        string_view text = line;
        if (current != nullptr) {
            if (startsWith(text, ")")) {
                current = nullptr;
            } else if (auto column = readColumn(text)) {
                current->columns.emplace_back(move(*column));
            }
            continue;
        }
        if (startsWith(text, createTable)) {
            auto name = text.substr(createTable.size());
            auto end = name.find(' ');
            current = &schema[string(name.substr(0, end))];
            continue;
        }
        if (startsWith(text, alterTable)) {
            altered = string(trim(text.substr(alterTable.size())));
            continue;
        }
        auto primaryKey = text.find(" PRIMARY KEY (");
        if (!altered.empty() && primaryKey != string_view::npos) {
            auto columns = text.substr(primaryKey + string_view(" PRIMARY KEY (").size());
            columns = columns.substr(0, columns.find(')'));
            auto table = schema.find(altered);
            if (table != schema.end()) {
                while (!columns.empty()) {
                    auto comma = columns.find(',');
                    table->second.primaryKey.emplace_back(trim(columns.substr(0, comma)));
                    columns = comma == string_view::npos ? string_view() : columns.substr(comma + 1);
                }
            }
        }
        if (!startsWith(text, " ")) {
            altered.clear();
        }
    }
    if (schema.empty()) {
        return nullopt;
    }
    return schema;
}

} // namespace sorbet::plugins::rails
