#ifndef SORBET_PLUGINS_RAILS_SOURCE_H
#define SORBET_PLUGINS_RAILS_SOURCE_H

// Reading a Rails app's Ruby files: where the app is, Prism parses of its files, the classes and modules they define,
// and Zeitwerk's mapping from a constant to its file.
//
// Files other than the one Sorbet is rewriting are read from disk on every call: rewrites run on many threads at
// once, and the language server must see a file as soon as it's saved, so nothing is cached.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "prism.h"
}

namespace sorbet::plugins::rails {

// A constant's full name, outermost first: `DataMigrations::Export::BaseJob` is {"DataMigrations", "Export",
// "BaseJob"}.
using ConstantName = std::vector<std::string>;

std::string join(const ConstantName &name);

// Where the app's files are, worked out from the path of a file Sorbet is checking: the part before `app/<dir>/`
// (or `lib/`). `srb tc` and the language server give paths relative to the app (`./app/jobs/x.rb`), the plugin tests
// absolute ones. The erb_views generator type-checks copies of some controllers in a temporary directory that has no
// other app files, so a file that isn't under the root is looked for in the working directory as well.
struct App {
    std::string root; // "" or a directory ending in '/'

    static std::optional<App> forPath(std::string_view path);

    // The first of root + relative and ./relative that exists.
    std::optional<std::string> find(std::string_view relative) const;
    // The names of the entries in a directory of the app, under the root first, then the working directory.
    std::vector<std::string> list(std::string_view relativeDir) const;
    // The path relative to the app, if `path` is under the root.
    std::optional<std::string> relative(std::string_view path) const;
};

// A Ruby file parsed with Prism.
class RubyFile {
public:
    // Parses `source` (the in-memory source Sorbet is checking).
    RubyFile(std::string path, std::string source);
    // Reads and parses a file from disk; nullptr if it doesn't exist.
    static std::shared_ptr<RubyFile> read(const std::string &path);
    ~RubyFile();
    RubyFile(const RubyFile &) = delete;
    RubyFile &operator=(const RubyFile &) = delete;

    const std::string path;
    const pm_node_t *root() const {
        return root_;
    }
    std::string_view name(pm_constant_id_t id) const;
    std::string_view slice(const pm_location_t &loc) const;
    // The line of a node, for error messages.
    int line(const pm_node_t *node) const;

private:
    std::string source;
    pm_parser_t parser;
    pm_node_t *root_;
};

// A constant reference as written: `Foo::Bar` or `::Foo::Bar` (absolute).
struct ConstantRef {
    ConstantName name;
    bool absolute = false;
};

// A constant reference (`Foo`, `Foo::Bar`, `::Foo`), or nullopt for anything else (`foo::Bar`, `self.class`).
std::optional<ConstantRef> constantRef(const RubyFile &file, const pm_node_t *node);

// A class or module definition in a parsed file.
struct Definition {
    std::shared_ptr<RubyFile> file;
    const pm_node_t *node; // PM_CLASS_NODE or PM_MODULE_NODE
    bool isClass;
    ConstantName fullName;
    // The lexical scopes for constant lookup inside the body, innermost first (Ruby's `Module.nesting`).
    std::vector<ConstantName> nesting;
    // The definitions it's nested in, outermost first, and the name each one was written with.
    std::vector<std::pair<bool, ConstantRef>> enclosing; // (isClass, written name)
    ConstantRef written;
    const pm_node_t *superclass; // a class's superclass expression, or nullptr
    std::vector<const pm_node_t *> body;

    // `include X` (or `extend X`) statements in the body, with literal constants, in order.
    std::vector<ConstantRef> mixins(std::string_view method) const;
    // `def name` (or `def self.name`) in the body, and the `sig` statement right before it, if any.
    struct Method {
        const pm_def_node_t *def;
        const pm_call_node_t *sig;
    };
    std::optional<Method> method(std::string_view name, bool singleton = false) const;
};

std::vector<Definition> definitions(const std::shared_ptr<RubyFile> &file);
std::optional<Definition> findDefinition(const std::shared_ptr<RubyFile> &file, const ConstantName &fullName);

// The constant that `ref`, written in a body with lexical scopes `nesting` (innermost first), refers to, and its
// definition in the file Zeitwerk would load it from. Tries each lexical scope innermost first, then the top level,
// as Ruby does (it doesn't look in ancestors). Only finds constants that have their own file in one of the app's
// autoload directories (`app/<dir>/`, `app/<dir>/concerns/`, `lib/`, or `app/` for a directory with a namespace of
// its own, like `app/billing/` for `Billing`). A file name matches a constant when the two are equal ignoring case
// and underscores, so `OAuth` matches `oauth` without our acronym list.
std::optional<Definition> resolve(const App &app, const std::vector<ConstantName> &nesting, const ConstantRef &ref);

// Prism helpers.
std::string_view callName(const RubyFile &file, const pm_call_node_t *call);
std::vector<const pm_node_t *> arguments(const pm_call_node_t *call);
// The statements of a body (a PM_STATEMENTS_NODE, or a begin block's statements).
std::vector<const pm_node_t *> statements(const pm_node_t *body);
std::string stringValue(const pm_string_t &string);

} // namespace sorbet::plugins::rails

#endif
