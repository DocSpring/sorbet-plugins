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
| `rails` | Writes the methods Rails and Sidekiq define at runtime from what the app's own code says, in place of Tapioca's DSL RBIs for them. Jobs: `perform_async`, `perform_in` and `perform_at` for Sidekiq jobs, `perform_later` and `perform_now` for ActiveJob jobs, typed from the job's `perform` and its sig (its own, an included module's or a parent class's). Controllers and mailers: the `HelperMethods` module Rails builds for their views (the parent's, the default helper module, `helper :all` for a direct `ActionController::Base` subclass, `helper` modules, and each `helper_method` with its method's sig, concerns' `included do` blocks included), and a controller's `helpers` and `HelperProxy`. It reads the files it needs (a job's parent class, an included module, a concern, a helper module) by their Zeitwerk paths, so nothing is generated. On code it can't type exactly (an anonymous parameter, a generic sig, a `helper` block) it reports an error rather than guessing. | |

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
exists to solve. So it reads those files from disk with Prism, each time, by Zeitwerk path, and checks that the file
it found defines the constant. The cost: the language server sees a change to one of those files only when it checks
the dependent file again, and Sorbet's `--cache-dir` (keyed on a file's own source) would keep a dependent file's old
types, so don't use it with this plugin. A plugin can report an error with `PluginError` (`plugins/Plugin.h`) when
it meets code it can't type exactly.

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
an app.
