# frozen_string_literal: true

# Writes Rails' strict locals comment, and a sig comment that types the locals, at the top of each partial in
# app/views that has no locals comment yet, from what the render graph and the type probe see at its renders:
#
#   <%# locals: (template:, show_actions: nil) -%>
#   <%# sig { params(template: Template, show_actions: T.nilable(T::Boolean)) } -%>
#
# A local every render passes is required, and one that only some pass, or that the partial reads through
# `local_assigns`, defaults to nil (Rails raises for a local a partial doesn't declare, so the comment declares every
# one a render may pass). A collection's `_counter` and `_iteration`, which Rails leaves out when a partial doesn't
# declare them, are declared only if the partial reads them. A local's type is the union of what the renders pass
# that Sorbet could type, nilable when one leaves it out; the sig leaves out a local no render passes a typed value
# for. A partial no render in the app reaches gets `locals: ()`.
#
# It leaves a partial alone, and lists it, when a gem renders it (an override of Kaminari's or Devise's partials) or
# no render reaches it but it reads `local_assigns`: write those comments by hand. It also lists optional locals that
# share a name with a helper: without strict locals a render that leaves one out calls the helper, and now the name
# is nil. Run it inside the app, then regenerate the RBI and type-check:
#
#   RAILS_ENV=development bin/rails runner path/to/add_locals.rb [--dry-run] [app/views/x/_y.html.erb ...]
#
# It types the locals with the app's Sorbet binary, as generate_rbi.rb does (SRB_SORBET_EXE).

require_relative 'generator/app'
require_relative 'generator/partial_locals'
require_relative 'generator/rbi'
require_relative 'generator/render_graph'
require_relative 'generator/types'

dry_run = ARGV.delete('--dry-run')
only = ARGV.map { |path| path.delete_prefix("#{Rails.root}/") }
sorbet = ENV.fetch('SRB_SORBET_EXE') { File.join(Gem::Specification.find_by_name('sorbet-static').gem_dir, 'libexec', 'sorbet') }
raise "no Sorbet binary at #{sorbet}" unless File.executable?(sorbet)

Rails.application.eager_load!
app = ErbViews::App.new(Rails.root)
graph = ErbViews::RenderGraph.new(app)
types = ErbViews::Types.new(app, graph, sorbet:)
helper_proxies = Dir.glob(File.join(Rails.root, 'sorbet/rbi/dsl/**/*.rbi')).filter_map do |file|
  source = File.read(file)
  source[/^class (\S+)/, 1] if source.include?('class HelperProxy < ')
end
rbi = ErbViews::Rbi.new(app, graph, types, helper_proxies:)
types.infer! { rbi.render }

IMPLICIT = /_(counter|iteration)\z/

# Whether a template reads a local it doesn't assign first: a bare `name`, which Ruby reads as a call.
def reads?(program, name)
  stack = [program]
  until stack.empty?
    node = stack.pop
    return true if node.is_a?(Prism::CallNode) && node.receiver.nil? && node.arguments.nil? && node.block.nil? && node.name.to_s == name

    stack.concat(node.compact_child_nodes)
  end
  false
end

# `<%# ... -%>` lines, wrapped after a comma before 118 columns.
def comment(open, items, close)
  lines = [+"<%# #{open}"]
  items.each_with_index do |item, index|
    text = index == items.size - 1 ? item : "#{item},"
    if lines.last.end_with?('(') || lines.last.size + text.size + 1 <= 118
      lines.last << (lines.last.end_with?('(') ? text : " #{text}")
    else
      lines << (' ' * (open.size + 4)) + text
    end
  end
  lines.last << "#{close} -%>"
  lines.join("\n")
end

def strip_root(type) = type.gsub(/(?<![\w:])::(?=[A-Z])/, '')

written = []
skipped = []
warnings = []
helpers = (app.renderers.flat_map { |renderer| renderer.klass._helpers.instance_methods } + ActionView::Base.instance_methods).map(&:to_s).uniq
partials = app.templates.select { |logical, _| File.basename(logical).start_with?('_') }
partials.sort.each do |logical, files|
  files.each do |file|
    next if !only.empty? && !only.include?(file)

    source = File.read(File.join(app.root, file))
    next if ErbViews::PartialLocals.read(source)

    if app.view_paths.any? { |path| path != File.join(app.root, 'app/views') && Dir.glob(File.join(path, "#{logical}.*")).any? }
      skipped << [file, 'a gem renders it too']
      next
    end

    program = Prism.parse(ErbViews::TemplateRuby.call(source)).value
    calls = graph.contexts.key?(logical) ? graph.contexts[logical].calls : []
    read_keys = graph.template_facts(file).local_keys
    if calls.empty? && !read_keys.empty?
      skipped << [file, "no render reaches it, but it reads local_assigns[#{read_keys.map(&:inspect).join(', ')}]"]
      next
    end

    passed = calls.flat_map { |call| call.locals.keys }.uniq
    keys = passed.reject { |key| key.match?(IMPLICIT) && !reads?(program, key) } + read_keys
    keys = keys.uniq.select { |key| key.match?(/\A[a-z_][a-zA-Z0-9_]*\z/) }
    required, optional = keys.partition do |key|
      calls.any? && calls.all? { |call| call.locals.key?(key) && call.locals[key] != ErbViews::RenderGraph::MAYBE }
    end
    params = required.sort.map { |key| "#{key}:" } + optional.sort.map { |key| "#{key}: nil" }
    typed = (required.sort + optional.sort).filter_map do |key|
      type = passed.include?(key) ? types.local_type(logical, key, sig: true) : 'T.untyped'
      "#{key}: #{strip_root(type)}" unless type == 'T.untyped'
    end

    header = comment('locals: (', params, ')')
    header += "\n#{comment('sig { params(', typed, ') }')}" unless typed.empty?
    written << [file, header]
    File.write(File.join(app.root, file), "#{header}\n#{source}") unless dry_run
    warn "erb_views: #{file}\n#{header}\n\n" if dry_run
    # Without strict locals, a local a render leaves out isn't a local, so the name calls the helper. Now it's nil.
    shadowed = optional.select { |key| helpers.include?(key) && reads?(program, key) }
    warnings << [file, "optional locals that are also helpers: #{shadowed.join(', ')}"] unless shadowed.empty?
  end
end

warn "erb_views: #{dry_run ? 'would write' : 'wrote'} locals comments in #{written.size} partials"
unless skipped.empty?
  warn 'erb_views: write these by hand:'
  skipped.each { |file, reason| warn "  #{file}: #{reason}" }
end
unless warnings.empty?
  warn 'erb_views: check these:'
  warnings.each { |file, warning| warn "  #{file}: #{warning}" }
end
