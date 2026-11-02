# frozen_string_literal: true

# Writes the RBI that the erb_views Sorbet plugin's template classes inherit: a context per template, with the view's
# helpers, the instance variables its actions assign (and their types) and its locals. Run it inside the app:
#
#   RAILS_ENV=development bin/rails runner path/to/generate_rbi.rb sorbet/rbi/plugins/erb_views.rbi path/to/sorbet
#
# The Sorbet binary must have the erb_views plugin: the types come from type-checking copies of the app's code
# (generator/types.rb). bin/sorbet-plugin-rbis runs it.

require_relative 'generator/app'
require_relative 'generator/render_graph'
require_relative 'generator/rbi'
require_relative 'generator/types'

output, sorbet = ARGV
raise 'usage: generate_rbi.rb <output.rbi> <sorbet binary>' if output.nil? || sorbet.nil?
raise "no Sorbet binary at #{sorbet}" unless File.executable?(sorbet)

Rails.application.eager_load!
app = ErbViews::App.new(Rails.root)
graph = ErbViews::RenderGraph.new(app)
types = ErbViews::Types.new(app, graph, sorbet:)

# The controllers whose Tapioca RBI has a HelperProxy class (a controller without one uses its parent's).
helper_proxies = Dir.glob(File.join(Rails.root, 'sorbet/rbi/dsl/**/*.rbi')).filter_map do |file|
  source = File.read(file)
  source[/^class (\S+)/, 1] if source.include?('class HelperProxy < ')
end

rbi = ErbViews::Rbi.new(app, graph, types, helper_proxies:)
types.infer! { rbi.render }
File.write(output, rbi.render)
