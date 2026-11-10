#ifndef SORBET_PLUGINS_RAILS_RAILS_H
#define SORBET_PLUGINS_RAILS_RAILS_H

#include "Copy.h"
#include "Source.h"
#include "Tree.h"

#include <map>
#include <set>

namespace sorbet::plugins::rails {

enum class Kind { None, Controller, Mailer };

// A class's place among controllers or mailers.
struct Lineage {
    Kind kind = Kind::None;
    // Its superclasses that are app classes, nearest first.
    std::vector<Definition> parents;
    // The farthest of them (or the class itself) is directly under ActionController::Base or ActionMailer::Base.
    bool directBase = false;
};

// A class whose superclass chain reaches ActionController::Base or ActionMailer::Base through app classes, or ends
// at a gem's controller or mailer (by its name's suffix); Kind::None for any other class.
Lineage lineage(const App &app, const Definition &def);

bool endsWith(std::string_view text, std::string_view suffix);

// A model: an app class whose superclass chain reaches ActiveRecord::Base through app classes, and isn't abstract.
struct Model {
    // The class, then its superclasses up to the one directly under ActiveRecord::Base.
    std::vector<Definition> chain;
    // Where its base class is in the chain: itself, or for an STI subclass, the farthest model above it.
    size_t base = 0;
};

// The class and its superclasses up to the one directly under ActiveRecord::Base, if every one is an app class.
std::optional<std::vector<Definition>> recordChain(const App &app, const Definition &def);
std::optional<Model> readModel(const App &app, const Definition &def);

// The app concerns a class or module includes, each one's own before it, once each: the order ActiveSupport::Concern
// includes them in the class that includes `def`.
std::vector<Definition> includedConcerns(const App &app, const Definition &def);

class Inflector;

// A macro in a model's code the plugin can't read, where it was written.
struct MacroProblem {
    std::shared_ptr<RubyFile> file;
    const pm_node_t *node;
    std::string message;
};

struct Table;

// One change a model macro makes to the model's attributes, as ModelMacros.cc reads them. Rails makes them in order:
// a superclass's first, then each class's in its body's order.
struct AttributeChange {
    enum class Kind {
        Declare,   // `attribute :name` with no type, and the ones `enum` and `serialize` make: keeps the type
        Type,      // `attribute :name, :type`
        Enum,      // `enum :name, ...`: the type becomes the enum's, around the one before it
        Serialize, // `serialize :name, ...`: the same, with the serialized class
        Alias,     // `alias_attribute :name, :old`
    };
    Kind kind;
    std::string name;
    // Type: the type's symbol (`boolean`). Serialize: `Array` or `Hash` for YAML of that class, or empty for any other
    // serializer. Alias: the attribute it's an alias of.
    std::string detail;
    bool array = false; // Type: `array: true`
    std::shared_ptr<RubyFile> file;
    const pm_node_t *node;
};

// The types of `attribute :name, :type` the plugin writes: the symbols for the column types it reads.
bool knownAttributeType(std::string_view symbol);

// RBI-shaped source for the model's GeneratedAttributeMethods (ModelColumns.cc): the methods Tapioca's
// ActiveRecordColumns compiler wrote for each attribute, from the table's columns (`primaryKey` is its primary key
// column, if it has one) without the ignored ones, and the macros' changes.
std::string attributeMethods(const Model &model, const Table &table, const std::optional<std::string> &primaryKey,
                             const std::set<std::string, std::less<>> &ignoredColumns,
                             const std::vector<AttributeChange> &changes, std::vector<MacroProblem> &problems);

// A `belongs_to`, `has_one`, `has_many` or `has_and_belongs_to_many` call in a model's code, as ModelMacros.cc reads
// it: the options that decide its methods' types.
struct Association {
    enum class Macro { BelongsTo, HasOne, HasMany, HasAndBelongsToMany };
    Macro macro = Macro::BelongsTo;
    std::string name = {};
    std::optional<std::string> className = {};
    bool polymorphic = false;
    std::optional<std::string> through = {};
    std::optional<std::string> source = {};
    std::optional<std::string> sourceType = {};
    // The class whose body declared it, or that includes the concern whose `included` block did: Rails looks its
    // class up from there. Its place in the model's chain.
    ConstantName owner = {};
    size_t level = 0;
    std::shared_ptr<RubyFile> file = {};
    const pm_node_t *node = nullptr;

    bool collection() const {
        return macro == Macro::HasMany || macro == Macro::HasAndBelongsToMany;
    }
    // `has_many :name`, as error messages name it.
    std::string declaration() const;
};

// A model's associations as Rails has them once its classes are loaded.
struct ModelAssociations {
    Model model;
    // For each class of the chain (the model first), the associations it has: its superclasses' and its own, by name,
    // where a later call with the same name replaces an earlier one.
    std::vector<std::map<std::string, Association, std::less<>>> levels;
    // The names `accepts_nested_attributes_for` was given in the chain.
    std::vector<std::string> nestedAttributes;
};

// A model's associations, reading its code and its concerns' (ModelMacros.cc). Problems with the macros go in
// `problems`.
ModelAssociations modelAssociations(const App &app, const Model &model, std::vector<MacroProblem> &problems);

// RBI-shaped source for the model's GeneratedAssociationMethods (ModelAssociations.cc): the methods Tapioca's
// ActiveRecordAssociations compiler wrote for its associations and nested attributes, typed with the classes Rails
// finds for them. Empty if it has none.
std::string associationMethods(const App &app, const Inflector &inflector, const ModelAssociations &associations,
                               std::vector<MacroProblem> &problems);

// Where the model's class body includes one of the modules modelMacros writes for its Shrine attachments: where Ruby
// includes the attachment's own module. After `after` (a statement of the model's body: the attachment's `include`,
// or the `include` of the concern whose `included` block has it), first in the body (a superclass's attachment), or
// else at the end.
struct AttachmentInclude {
    std::string module; // inside the model
    const pm_node_t *after = nullptr;
    bool first = false;
};

// RBI-shaped source (for addTemplate) for the methods of the `scope`, `enum` and `store_accessor` calls in a model's
// code, its associations, its attributes and its Shrine attachments (ModelMacros.cc), and the macros it couldn't read.
std::string modelMacros(const App &app, const Model &model, const Inflector &inflector, const Table &table,
                        const std::optional<std::string> &primaryKey, std::vector<MacroProblem> &problems,
                        std::vector<AttachmentInclude> &attachmentIncludes);

// Shrine (Uploaders.cc). The attachment methods a plugin defines (`activerecord` brings `model`'s and `entity`'s).
namespace ShrineAttachment {
enum : uint8_t {
    Entity = 1 << 0,      // `name`, `name_attacher`, `name_url`
    Model = 1 << 1,       // `name=`, `name_changed?`
    CachedData = 1 << 2,  // `cached_name_data`
    Derivatives = 1 << 3, // `name_derivatives`, `name_derivatives!`, and `name` takes a derivative's name
};
}

// What an uploader's plugins (its own, its superclasses' and Shrine's global ones) give its attachments.
struct UploaderPlugins {
    ConstantName uploader;
    uint8_t attachment = 0;
    bool versions = false; // a plugin's `configure` sets `opts[:versions]`: the reader can return a hash of versions
};

// The plugins of an uploader (an app class whose superclasses up to Shrine are the app's), or nullopt with `error`.
std::optional<UploaderPlugins> uploaderPlugins(const App &app, const Definition &def, std::string &error);

// RBI-shaped source for the methods `include Uploader.attachment(:name)` defines, without those `skip` names.
std::string attachmentMethods(const UploaderPlugins &plugins, std::string_view name,
                              const std::function<bool(std::string_view)> &skip);

// Each part runs once per file, from the file's `<root>` body, after Sorbet has rewritten every class in it.
void rewriteJobs(core::MutableContext ctx, ast::ClassDef &root, const App &app, const std::shared_ptr<RubyFile> &file);
void rewriteHelpers(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                    const std::shared_ptr<RubyFile> &file);
void rewriteMailers(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                    const std::shared_ptr<RubyFile> &file);
void rewriteConcerns(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                     const std::shared_ptr<RubyFile> &file);
void rewriteModels(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                   const std::shared_ptr<RubyFile> &file);
void rewriteComponents(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                       const std::shared_ptr<RubyFile> &file);
void rewriteUploaders(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                      const std::shared_ptr<RubyFile> &file);

} // namespace sorbet::plugins::rails

#endif
