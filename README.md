# Sorbet rewriter plugins

Plugins for [DocSpring's Sorbet build](https://github.com/DocSpring/sorbet/tree/docspring) that teach Sorbet the
Ruby DSLs whose blocks it otherwise can't type. Each one rewrites a file's syntax tree into shapes Sorbet already
understands, before Sorbet's own rewriters run. A plugin sees one file at a time; what it needs to know about the app
(spec types, factories, ActiveAdmin resources) comes from an RBI that its generator writes from the running app.

| Plugin | What it does | Generator |
| --- | --- | --- |
| `rspec` | Includes the modules RSpec's configuration adds to each spec type (`type:` or inferred from the path) in every top-level group and shared example group. Turns `[...].each do \|x\| it ... end` into Sorbet's `test_each`, and loops it can't take into groups. Flattens class-level conditionals around examples. Types `\|example\|` in `it`, `around`. | `rspec/generate_rbi.rb` (loads `rails_helper`, records what each spec type's groups get) |
| `capybara` | `feature`/`scenario`/`background`/`given` as `describe`/`it`/`before`/`let`, with `type: :feature`. | |
| `rswag` | `path`, HTTP verbs and `response` as groups and `run_test!` as an example, inside spec groups. | |
| `factory_bot` | `create(:user, :admin, name: 'x')` as `FactoryBotTypes::User.create(...)`, so factory, trait and attribute names are checked and the result is typed; `let(:user) { create(:user) }` gets a sig. `typed: true` files only. | `factory_bot/generate_rbi.rb` |
| `active_admin` | Binds each block of `ActiveAdmin.register` to what runs it: the registration DSL, a view with the controller's helpers, or the controller. Moves `controller do` and action blocks into a stand-in controller class, and types `resource` and the resource's block params. | `active_admin/generate_rbi.rb` |

## Using them

Enable plugins on the command line or in `sorbet/config`:

```
--enable-experimental-rspec
--enable-plugin=rspec
--enable-plugin=capybara
--enable-plugin=rswag
--enable-plugin=factory_bot
--enable-plugin=active_admin
```

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
