# frozen_string_literal: true

# Writes the RBIs for the erb_views Sorbet plugin's template classes, in a directory that mirrors the app: the
# contexts the classes inherit, and for each template its renderers' helpers, the instance variables it uses (with
# their types) and its locals (generator/rbi.rb). Run it inside the app:
#
#   RAILS_ENV=development bin/rails runner path/to/generate_rbi.rb sorbet/rbi/erb_views
#
# It rewrites only the files whose text changes, and deletes the ones for templates that are gone. The test
# environment checks each render against the same types (generator/runtime_check.rb).
#
# The types come from type-checking copies of the app's code (generator/types.rb) with the sorbet-static gem's Sorbet
# binary, or SRB_SORBET_EXE (as for `srb`). It must have the erb_views plugin. A script that runs it can skip it when
# nothing it depends on changed (generator/inputs.rb), as the app's bin/sorbet-plugin-rbis does.

require_relative 'generator/app'
require_relative 'generator/inputs'
require_relative 'generator/render_graph'
require_relative 'generator/rbi'
require_relative 'generator/types'

output = ARGV.fetch(0) { raise 'usage: generate_rbi.rb <output directory>' }
sorbet = ErbViews::Inputs.sorbet

Rails.application.eager_load!
app = ErbViews::App.new(Rails.root)
graph = ErbViews::RenderGraph.new(app)
types = ErbViews::Types.new(app, graph, sorbet:, rbi_dir: app.relative(File.expand_path(output, Rails.root)))

# The controllers and mailers whose Tapioca RBI declares their own HelperMethods module (one without uses its
# parent's).
helper_modules = Dir.glob(File.join(Rails.root, 'sorbet/rbi/dsl/**/*.rbi')).filter_map do |file|
  source = File.read(file)
  source[/^class (\S+)/, 1] if source.match?(/^  module HelperMethods$/)
end

rbi = ErbViews::Rbi.new(app, graph, types, helper_modules:)
types.infer! { rbi.combined }
ErbViews::Rbi.write(output, rbi.files)
# What the RBIs can't be right without (a declaration its renderers contradict, a render Rails resolves elsewhere):
# fail, with the RBIs written.
problems = rbi.problems
abort problems.map { |problem| "erb_views: #{problem}" }.join("\n") unless problems.empty?
