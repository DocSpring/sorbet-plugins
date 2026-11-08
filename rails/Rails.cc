// The rails plugin: types for what a Rails app's own code declares, written while Sorbet reads the file, in place of
// the Tapioca DSL RBIs that copied the same facts and went stale whenever the code changed.
//
// It reads other files of the app by Zeitwerk path (a job's parent, an included module), and for models
// `db/structure.sql` and `config/initializers/inflections.rb`, from disk, every time. The language server sees a
// change to one of those files only when the file that depends on it is checked again (edit it, or restart the
// server). Don't use `--cache-dir` with this plugin: Sorbet's cache is keyed on a file's own
// source, so it would keep a dependent file's old types.

#include "Rails.h"
#include "plugins/Plugin.h"

using namespace std;

namespace sorbet::plugins {
namespace {

class RailsPlugin final : public Plugin {
public:
    string_view name() const override {
        return "rails";
    }

    void rewriteClassBody(core::MutableContext ctx, ast::ClassDef &classDef) const override {
        if (classDef.symbol != core::Symbols::root()) {
            return;
        }
        auto path = ctx.file.data(ctx).path();
        auto app = rails::App::forPath(path);
        if (!app) {
            return;
        }
        auto relative = app->relative(path);
        if (!relative) {
            return;
        }
        auto isUnder = [&](string_view dir) { return relative->substr(0, dir.size()) == dir; };
        auto source = ctx.file.data(ctx).source();
        auto jobs = isUnder("app/jobs/");
        auto helpers = isUnder("app/controllers/") || isUnder("app/mailers/");
        // A concern's file names ActiveSupport::Concern: the text check saves parsing every other file.
        auto concerns =
            (isUnder("app/") || isUnder("lib/")) && source.find("ActiveSupport::Concern") != string_view::npos;
        // A model's file has a class with a superclass. Controllers, jobs, mailers, views and the rest never hold one.
        auto models = isUnder("app/") && !jobs && !helpers && !isUnder("app/views/") && !isUnder("app/helpers/") &&
                      !isUnder("app/admin/") && !isUnder("app/components/") && source.find(" < ") != string_view::npos;
        // A component's slots: ViewComponent's `renders_one` and `renders_many`.
        auto components = isUnder("app/") && (source.find("renders_one") != string_view::npos ||
                                              source.find("renders_many") != string_view::npos);
        if (!jobs && !helpers && !concerns && !models && !components) {
            return;
        }
        auto file = make_shared<rails::RubyFile>(string(path), string(source));
        if (concerns) {
            rails::rewriteConcerns(ctx, classDef, *app, file);
        }
        if (jobs) {
            rails::rewriteJobs(ctx, classDef, *app, file);
        }
        if (helpers) {
            rails::rewriteHelpers(ctx, classDef, *app, file);
        }
        if (isUnder("app/mailers/")) {
            rails::rewriteMailers(ctx, classDef, *app, file);
        }
        if (models) {
            rails::rewriteModels(ctx, classDef, *app, file);
            // Shrine uploaders are classes with a superclass too.
            rails::rewriteUploaders(ctx, classDef, *app, file);
        }
        if (components) {
            rails::rewriteComponents(ctx, classDef, *app, file);
        }
    }
};

} // namespace
SORBET_PLUGIN(RailsPlugin);
} // namespace sorbet::plugins
