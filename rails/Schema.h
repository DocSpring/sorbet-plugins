#ifndef SORBET_PLUGINS_RAILS_SCHEMA_H
#define SORBET_PLUGINS_RAILS_SCHEMA_H

// The app's database tables, read from `db/structure.sql` (pg_dump's output), so a model's types come from the
// committed schema rather than whatever a local database has migrated to.

#include "Source.h"

#include <map>

namespace sorbet::plugins::rails {

struct Column {
    std::string name;
    std::string sqlType; // as pg_dump writes it, `[]` included: `timestamp(6) without time zone`, `character varying[]`
    bool notNull = false;
};

struct Table {
    std::vector<Column> columns;
    std::vector<std::string> primaryKey; // empty for a table without one
    const Column *column(std::string_view name) const;
};

using Schema = std::map<std::string, Table, std::less<>>;

// The tables in the app's `db/structure.sql` (in the `public` schema), or nullopt if it has none.
std::optional<Schema> readSchema(const App &app);

} // namespace sorbet::plugins::rails

#endif
