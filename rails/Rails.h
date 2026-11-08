#ifndef SORBET_PLUGINS_RAILS_RAILS_H
#define SORBET_PLUGINS_RAILS_RAILS_H

#include "Copy.h"
#include "Source.h"
#include "Tree.h"

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

// Each part runs once per file, from the file's `<root>` body, after Sorbet has rewritten every class in it.
void rewriteJobs(core::MutableContext ctx, ast::ClassDef &root, const App &app, const std::shared_ptr<RubyFile> &file);
void rewriteHelpers(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                    const std::shared_ptr<RubyFile> &file);
void rewriteMailers(core::MutableContext ctx, ast::ClassDef &root, const App &app,
                    const std::shared_ptr<RubyFile> &file);

} // namespace sorbet::plugins::rails

#endif
