# Sorbet rewriter plugins

Plugins for [DocSpring's Sorbet build](https://github.com/DocSpring/sorbet/tree/docspring) that teach Sorbet the
Ruby DSLs whose blocks it otherwise can't type, Rails' ERB templates, and the methods Rails and Sidekiq define from an
app's own code. Each one rewrites a file's syntax tree into shapes Sorbet already understands, before Sorbet's own
rewriters run. Most plugins see one file at a time; what they need to know about the app (spec types, factories,
ActiveAdmin resources, what each template's controllers set) comes from an RBI that their generator writes from the
running app. `rails` reads the app's other files itself (below).

The build's [README](https://github.com/DocSpring/sorbet/blob/docspring/docspring/README.md) covers what it changes
from upstream Sorbet, how it's released, and where the gems are served from.

| Plugin | What it does | Generator |
| --- | --- | --- |
| `rspec` | Includes the modules RSpec's configuration adds to each spec type (`type:` or inferred from the path) in every top-level group and shared example group. Turns `[...].each do \|x\| it ... end` into Sorbet's `test_each`, and loops it can't take into groups. Flattens class-level conditionals around examples. Types `\|example\|` in `it`, `around`. Checks test-prof's `let_it_be` as a `let` and `before_all` as a `before` hook. | `rspec/generate_rbi.rb` (loads `rails_helper`, records what each spec type's groups get) |
| `capybara` | `feature`/`scenario`/`background`/`given` as `describe`/`it`/`before`/`let`, with `type: :feature`. | |
| `rswag` | `path`, HTTP verbs and `response` as groups and `run_test!` as an example, inside spec groups. | |
| `factory_bot` | `create(:user, :admin, name: 'x')` as `FactoryBotTypes::User.create(...)`, so factory, trait and attribute names are checked and the result is typed; `let(:user) { create(:user) }` gets a sig. In factory definitions, binds each `factory :user do` block to `FactoryBotTypes::User::Definition`, so attribute and trait names are checked, attribute blocks see the record's methods and the transient attributes, and callbacks get a typed record. `typed: true` files only. | `factory_bot/generate_rbi.rb` |
| `active_admin` | Binds each block of `ActiveAdmin.register` to what runs it: the registration DSL, a view with the controller's helpers, or the controller. Moves `controller do` and action blocks into a stand-in controller class (action blocks as methods with an implied `sig { void }`, so they pass at `typed: strict`), and types `resource` and the resource's block params. | `active_admin/generate_rbi.rb` |
| `erb_views` | Reads each template in `app/views` and `app/components` as the Ruby in its tags, at `typed: strict`, in a method of a class named after its path. A view's class inherits a controller's helpers, or a mailer's for a view in a mailer's directory, so a new view type-checks before anything is generated; the template's own RBI adds its renderers' helpers, its instance variables and their types, and its locals. A partial's locals come from Rails' strict locals comment, typed by a `sig` comment; each partial declares the method its renders call, and a `render` of a literal partial name calls it, so its locals are checked. | `erb_views/generate_rbi.rb` writes an RBI per template, in a directory that mirrors the app (it follows each action to what it renders, and asks Sorbet each ivar's type there). `erb_views/generator/runtime_check.rb` checks every render against the same types in the test environment. `erb_views/add_locals.rb` writes the locals and `sig` comments for existing partials. |
| `rails` | Writes the methods Rails and Sidekiq define at runtime from what the app's own code says, in place of Tapioca's DSL RBIs for them. Jobs: `perform_async`, `perform_in` and `perform_at` for Sidekiq jobs, `perform_later` and `perform_now` for ActiveJob jobs, typed from the job's `perform` and its sig (its own, the last included module's or a parent class's; an ActiveJob job types only its own), its kind from an `include Sidekiq::Job` (directly or through an app module) or an `ApplicationJob` ancestor. Controllers and mailers: the `HelperMethods` module Rails builds for their views (the parent's, the default helper module, `helper :all` for a direct `ActionController::Base` subclass, `helper` modules, and each `helper_method` (`helper_method def foo` too) with its method's sig, concerns' `included do` blocks included, with what a gem parent gives in an empty `SorbetInheritedHelperMethods` module, included first, that the app's Tapioca compiler fills), and a controller's `helpers` and `HelperProxy`. Mailers: each action (a public method the mailer defines or gets from an app module it includes) as a class method with the same parameters and sig types, returning `ActionMailer::MessageDelivery`. Concerns: the class and instance methods of each `class_attribute` (or `self.class_attribute`) in an `included` block (`GeneratedClassMethods` and `GeneratedInstanceMethods`, by Rails' rules for the `instance_*` options), and the class methods of the app concerns a concern includes, which Sorbet doesn't pass on to the class that includes it. Models (an app class whose superclasses up to `ActiveRecord::Base` are all the app's, and that isn't abstract): the relation methods Tapioca's ActiveRecordRelations compiler writes (`where`, `find`, the `PrivateRelation` classes), from a template of its output (`rails/templates/relations.rbi`), with `ids` typed from the primary key in `db/structure.sql` of the table Rails names with the app's inflections, the methods of the `scope`, `enum` and `store_accessor` calls in the model's code (its class bodies and its concerns' `included` blocks), with enum values from a literal (keywords too) or a constant set to one, a Symbol value typed as the String Rails stores, and the app's own enum macros (a class method that passes its first two parameters to `enum`) read with their option defaults, and the 19 methods Tapioca's ActiveRecordColumns compiler writes for each attribute (the columns of its table in `db/structure.sql` without `ignored_columns`, `id`, Rails' `id_value`, each `attribute` and `alias_attribute`), with Tapioca's types: from the column's SQL type and nullability, changed by `attribute :name, :type`, `enum`, `serialize` and the app's attribute macros (a class method that gives each name in its `*` parameter to `attribute` with a type) in the order Rails runs them, and the methods Tapioca's ActiveRecordAssociations compiler writes for each `belongs_to`, `has_one`, `has_many`, `has_and_belongs_to_many` and `accepts_nested_attributes_for`, typed with the class Rails finds (`class_name:` or the name, looked up in the declaring class's namespaces as `compute_type` does, a `through:` association's source class, `T.untyped` when polymorphic). ViewComponent components: the methods each `renders_one` and `renders_many` in a class body defines (`footer` and `footer?` in the class's `GeneratedSlotMethods`, as ViewComponent defines them, `with_footer`, `with_footer_content`, a collection's `with_footers`, and a polymorphic slot's `with_x` and `with_x_content` per type, named by its `as:`), untyped as the slot delegates to its component. Shrine uploaders (a class in `app/` or `lib/` whose superclasses up to `Shrine` are all the app's): the `Attacher` and `UploadedFile` subclasses `Shrine.inherited` creates, and the modules each `plugin` call in the class body (one under `if` or `unless` too) mixes into the three, in Shrine's order: Shrine's plugins from a table of Shrine 3.10's modules and dependencies, and the app's from their files with their `load_dependencies`. Shrine attachments: a model's `include Uploader.attachment(:name)` (or `T.unsafe(self).include ...`) gets the methods the plugins of the uploader, its superclasses and `config/initializers/shrine.rb` define for it (the reader returns the uploader's `UploadedFile`, or a hash of them when a plugin sets versions), without those the model defines itself, in a module of its own included where Ruby includes the attachment's, once for a name declared twice. It reads the files it needs (a job's parent class, an included module, a concern, a helper module, an uploader and its plugins, `db/structure.sql`, `config/initializers/inflections.rb`, `config/initializers/shrine.rb`) from disk, so nothing is generated, finding a constant's file as Zeitwerk does (through the app's inflections when two file names differ only in underscores). On code it can't type exactly (a method that takes `...`, a generic sig, a `helper` block, a `class_attribute` option that isn't `true` or `false`, a model whose table it can't find, a scope whose name is a variable, enum values it would have to run code to know, a column or `attribute` type it doesn't know, an association option that isn't a literal, a `through:` whose source it can't find, a class method that calls a model macro in a way it doesn't read, a slot given something other than a component, a lambda or `types:` written out, a Shrine plugin missing from its table, a `plugin` call or attachment it can't read, a job whose kind it can't tell, `clear_helpers`, a model macro called on a receiver or in `def self.included`) it reports an error rather than guessing. | |

## Using them

Enable plugins on the command line or in `sorbet/config`:

```
--allowed-extension=.rb,.rbi,.erb
--enable-experimental-rspec
--enable-plugin=capybara
--enable-plugin=rswag
--enable-plugin=rspec
--enable-plugin=factory_bot
--enable-plugin=active_admin
--enable-plugin=rails
--enable-plugin=erb_views
```

`--allowed-extension` is only for `erb_views`, which needs Sorbet to read `.erb` files. The plugins run in the order
listed: `capybara` and `rswag` turn their DSLs into RSpec's names, so they come before `rspec`.

Generate the plugins' RBIs inside the app (see the comment at the top of each `generate_rbi.rb`).

## Writing a plugin

A plugin is a Bazel repository with a `:plugin` `cc_library` (`alwayslink = True`) that depends on `@//plugins:api`
and registers itself:

```cpp
#include "plugins/Plugin.h"

class MyPlugin final : public sorbet::plugins::Plugin {
public:
    std::string_view name() const override { return "my_plugin"; }
    void rewriteClassBody(sorbet::core::MutableContext ctx, sorbet::ast::ClassDef &classDef) const override {
        // Rewrite classDef.rhs in place.
    }
};

SORBET_PLUGIN(MyPlugin);
```

`rewriteClassBody` runs for every class and module body (and the file's root), before Sorbet's statement rewriters.
It should be a pure function of the file: anything it needs from elsewhere goes in a generated RBI that the rewritten
code refers to by constant. Add the repository to `plugins/plugins.bzl` in the Sorbet build.

`rails` is the exception. The facts it needs (a job's parent class, a model's columns) are written in the app's
other files, and a generated copy of them goes stale the moment someone edits the original, which is the problem it
exists to solve. So it reads those files from disk each time: Ruby files with Prism, by Zeitwerk path (checking that
the file it found defines the constant), and `db/structure.sql`, the inflections initializer and the Shrine initializer.
The cost: the language server sees a change to one of those files only when it checks the dependent file again, and
Sorbet's `--cache-dir` (keyed on a file's own source) would keep a dependent file's old types, so don't use it with
this plugin. A plugin can report an error with `PluginError` (`plugins/Plugin.h`) when
it meets code it can't type exactly.

The methods `rails` writes for every model are many and the same for each, so they're kept as RBI-shaped Ruby
(`rails/templates/`, built into the plugin as strings) that it parses with Prism and turns into Sorbet's tree, `sig`s
and all. Tapioca's RBIs overload some of these methods (several `sig`s on one `def`), which Sorbet allows only in RBI
files; DocSpring's Sorbet build allows them for methods a rewriter wrote, and skips checking those methods' bodies.

A plugin that reads files in another language overrides two more hooks, as `erb_views` does:
`transformSource(path, source)` returns the Ruby to parse in place of the file's source, with exactly as many bytes
(so every location in it is a location in the file, and errors show the file's own text), and
`strictLevelWithoutSigil(path)` gives such a file's strictness when it has no `typed:` sigil.

Things we learned the hard way:

- Sorbet's own sends are already rewritten when a plugin sees the root class: build `T.bind`/`T.let` as `ast::Cast`
  nodes (`core::Names::syntheticBind()`, `MK::Let`), not as sends.
- `ast::TreeWalk` re-casts a node after the pre-transform hook, so don't replace the node there.
- An unsigned `let` returns `T.untyped` to its callers. To type one, put a sig before it (the factory_bot plugin
  does this for lets that build a record).

## Tests

Each plugin's `test/` has fixtures with `# error:` markers, checked against a Sorbet build. `rails` reads other
files, so each directory in `rails/test/` is a small app of its own, checked in one Sorbet run:

```sh
ruby test/run_tests.rb ../sorbet/bazel-bin/main/sorbet
```

It also runs a generator's own tests (`<plugin>/generator/test/*_test.rb`, Minitest), for the parts that don't need
an app, and `test/language_server_test.rb`, which edits files of the `rails` plugin's `jobs` app in Sorbet's language
server and checks that every file keeps its types on the fast path, the slow path and a preempted slow path.

## License

Apache License 2.0, the same as Sorbet: see [LICENSE](LICENSE).
