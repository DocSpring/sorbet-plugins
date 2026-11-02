# Sorbet rewriter plugins

Plugins for [DocSpring's Sorbet build](https://github.com/DocSpring/sorbet/tree/docspring) that teach Sorbet the
Ruby DSLs whose blocks it otherwise can't type, and Rails' ERB templates. Each one rewrites a file's syntax tree into
shapes Sorbet already understands, before Sorbet's own rewriters run. A plugin sees one file at a time; what it needs
to know about the app (spec types, factories, ActiveAdmin resources, what each template's controllers set) comes from
an RBI that its generator writes from the running app.

The build's [README](https://github.com/DocSpring/sorbet/blob/docspring/docspring/README.md) covers what it changes
from upstream Sorbet, how it's released, and where the gems are served from.

| Plugin | What it does | Generator |
| --- | --- | --- |
| `rspec` | Includes the modules RSpec's configuration adds to each spec type (`type:` or inferred from the path) in every top-level group and shared example group. Turns `[...].each do \|x\| it ... end` into Sorbet's `test_each`, and loops it can't take into groups. Flattens class-level conditionals around examples. Types `\|example\|` in `it`, `around`. Checks test-prof's `let_it_be` as a `let` and `before_all` as a `before` hook. | `rspec/generate_rbi.rb` (loads `rails_helper`, records what each spec type's groups get) |
| `capybara` | `feature`/`scenario`/`background`/`given` as `describe`/`it`/`before`/`let`, with `type: :feature`. | |
| `rswag` | `path`, HTTP verbs and `response` as groups and `run_test!` as an example, inside spec groups. | |
| `factory_bot` | `create(:user, :admin, name: 'x')` as `FactoryBotTypes::User.create(...)`, so factory, trait and attribute names are checked and the result is typed; `let(:user) { create(:user) }` gets a sig. In factory definitions, binds each `factory :user do` block to `FactoryBotTypes::User::Definition`, so attribute and trait names are checked, attribute blocks see the record's methods and the transient attributes, and callbacks get a typed record. `typed: true` files only. | `factory_bot/generate_rbi.rb` |
| `active_admin` | Binds each block of `ActiveAdmin.register` to what runs it: the registration DSL, a view with the controller's helpers, or the controller. Moves `controller do` and action blocks into a stand-in controller class (action blocks as methods with an implied `sig { void }`, so they pass at `typed: strict`), and types `resource` and the resource's block params. | `active_admin/generate_rbi.rb` |
| `erb_views` | Reads each template in `app/views` and `app/components` as the Ruby in its tags, at `typed: strict`, in a method of a class whose superclass is the template's context: the helpers of the controllers and mailers that render it, its instance variables and their types, and its locals. A partial's locals come from Rails' strict locals comment, typed by a `sig` comment, and a `render` of a literal partial name calls the partial's method, so its locals are checked. | `erb_views/generate_rbi.rb` (follows each action to what it renders, and asks Sorbet each ivar's type there; also writes a runtime check of the same types for the test environment). `erb_views/add_locals.rb` writes the locals and `sig` comments for existing partials. |

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
--enable-plugin=erb_views
```

`--allowed-extension` is only for `erb_views`, which needs Sorbet to read `.erb` files. The plugins run in the order
listed: `capybara` and `rswag` turn their DSLs into RSpec's names, so they come before `rspec`.

and generate their RBIs inside the app (see the comment at the top of each `generate_rbi.rb`).

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
It must be a pure function of the file: anything it needs from elsewhere goes in a generated RBI that the rewritten
code refers to by constant. Add the repository to `plugins/plugins.bzl` in the Sorbet build.

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

Each plugin's `test/` has fixtures with `# error:` markers, checked against a Sorbet build:

```sh
ruby test/run_tests.rb ../sorbet/bazel-bin/main/sorbet
```

It also runs a generator's own tests (`<plugin>/generator/test/*_test.rb`, Minitest), for the parts that don't need
an app.
