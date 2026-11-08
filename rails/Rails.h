#ifndef SORBET_PLUGINS_RAILS_RAILS_H
#define SORBET_PLUGINS_RAILS_RAILS_H

#include "Copy.h"
#include "Source.h"
#include "Tree.h"

namespace sorbet::plugins::rails {

// Each part runs once per file, from the file's `<root>` body, after Sorbet has rewritten every class in it.
void rewriteJobs(core::MutableContext ctx, ast::ClassDef &root, const App &app, const std::shared_ptr<RubyFile> &file);

} // namespace sorbet::plugins::rails

#endif
