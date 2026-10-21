# frozen_string_literal: true

# Writes the RBI that the rspec Sorbet plugin's `include ::RSpecConfig::...` lines refer to.
#
# Rather than reading `config.include`/`config.extend` calls, it asks RSpec: it defines a throwaway example group with
# no type and one per spec type, and records what each group's ancestors add. That covers rspec-rails' own type
# modules, modules that extend the group when they're included, and shared contexts included by metadata.
#
# Run it with the app's RSpec configuration loaded:
#
#   RAILS_ENV=test bundle exec ruby -Ispec -e 'require "rspec/core"; require "rails_helper"; load ARGV.shift' \
#     path/to/generate_rbi.rb sorbet/rbi/dsl/rspec_config.rbi [spec]

output = ARGV.fetch(0) { raise 'usage: generate_rbi.rb <output.rbi> [spec dir]' }
spec_dir = ARGV.fetch(1, 'spec')

def camelize(name) = name.to_s.split('_').reject(&:empty?).map { |part| part[0].upcase + part[1..] }.join

BASE = RSpec::Core::ExampleGroup
OBJECT_ANCESTORS = Object.ancestors
URL_HELPERS = defined?(Rails) ? Rails.application.routes.url_helpers : nil

config = RSpec.configuration
filter_types = %i[@include_modules @extend_modules @prepend_modules].flat_map do |ivar|
  config.instance_variable_get(ivar).items_and_filters.filter_map { |_, filter| filter[:type] }
end
spec_files = Dir.glob(File.join(spec_dir, '**/*.rb'))
# Types the specs use that no filter mentions (the plugin includes a module for every type it sees).
used_types = spec_files.flat_map { |path| File.read(path).scan(/\btype: :(\w+)/).flatten }
types = ((defined?(RSpec::Rails::DIRECTORY_MAPPINGS) ? RSpec::Rails::DIRECTORY_MAPPINGS.keys : []) + [:feature] +
         filter_types + used_types).map(&:to_sym).uniq.sort

PROJECT_ROOT = "#{File.expand_path(defined?(Rails) ? Rails.root.to_s : Dir.pwd)}/"
# Gems can live inside the project (vendor/bundle on CI, vendored gems), so a file under the root isn't necessarily
# the app's own.
GEM_DIRS = [*Gem.path, Bundler.bundle_path.to_s, "#{PROJECT_ROOT}vendor"].map { |dir| "#{File.expand_path(dir)}/" }

def app_file?(path)
  path = File.expand_path(path)
  path.start_with?(PROJECT_ROOT) && GEM_DIRS.none? { |dir| path.start_with?(dir) }
end

# Modules the app's own code mixed into Object (`include Helpers` at the top level of a support file): every example
# has them.
object_mixins = Object.ancestors.select do |mod|
  next false if mod.is_a?(Class) || mod.name.nil?

  location = Object.const_source_location(mod.name)&.first
  location && app_file?(location)
end

def define_group(metadata)
  group = RSpec.describe('Sorbet rspec plugin generator', **metadata) {}
  RSpec.world.example_groups.delete(group)
  group
end

untyped = define_group({})
by_type = types.to_h { |type| [type, define_group(type:)] }

# Modules a group gets beyond `others`, lowest precedence first (the order `include` lines need).
def added_modules(ancestors, others)
  (ancestors - others - OBJECT_ANCESTORS).reject { |mod| mod.is_a?(Class) }.reverse
end

# A module RBI can name. Each group's own modules (LetDefinitions, NamedSubjectPreventSuper) live under
# RSpec::ExampleGroups, which only exists at runtime.
def nameable?(mod) = mod.name && !mod.name.start_with?('RSpec::ExampleGroups::') && !mod.name.start_with?('#<')

SIMPLE_METHOD = /\A[a-z_][a-zA-Z0-9_]*[?!=]?\z/

def stub(name) = "      def #{name}(*args, **kwargs, &blk); end"

# Lines for a module body that brings in `mods`.
def module_lines(mods, extra_methods: [])
  lines = []
  stubs = []
  mods.each do |mod|
    if URL_HELPERS && mod.equal?(URL_HELPERS)
      lines << '      include GeneratedUrlHelpersModule'
      lines << '      include GeneratedPathHelpersModule'
    elsif nameable?(mod)
      lines << "      include ::#{mod.name}"
    else
      # Shared contexts included by metadata and other anonymous modules: their methods, untyped.
      stubs.concat(mod.public_instance_methods(false).map(&:to_s))
    end
  end
  stubs.concat(extra_methods)
  stubs.uniq.sort.grep(SIMPLE_METHOD).each { |name| lines << stub(name) }
  lines
end

# Request specs reach the integration session (`response`, `request`, ...) through Runner#method_missing.
def integration_session_methods(mods)
  return [] unless defined?(ActionDispatch::Integration::Runner) && mods.include?(ActionDispatch::Integration::Runner)

  (ActionDispatch::Integration::Session.public_instance_methods - Object.public_instance_methods -
    ActionDispatch::Integration::Runner.public_instance_methods).map(&:to_s)
end

# Request and controller specs reach the route helpers through method_missing (the integration session's, and
# ControllerExampleGroup's, which forwards them to the controller).
ROUTE_HELPERS_VIA_METHOD_MISSING = [
  defined?(ActionDispatch::Integration::Runner) && ActionDispatch::Integration::Runner,
  defined?(RSpec::Rails::ControllerExampleGroup) && RSpec::Rails::ControllerExampleGroup,
].compact

def route_helper_lines(mods)
  return [] unless URL_HELPERS && !mods.include?(URL_HELPERS) && mods.intersect?(ROUTE_HELPERS_VIA_METHOD_MISSING)

  ['      include GeneratedUrlHelpersModule', '      include GeneratedPathHelpersModule']
end

# RSpec's be_<predicate> and have_<x> matchers come from RSpec::Matchers#method_missing.
dynamic_matchers = spec_files.flat_map do |path|
  File.read(path).scan(/(?<![\w.])((?:be|have)_[a-z0-9_]+)\b/).flatten
end.uniq.sort.reject do |name|
  [untyped, *by_type.values].any? { |group| group.method_defined?(name) || group.private_method_defined?(name) }
end

lines = []
lines << '# typed: true'
lines << ''
lines << '# DO NOT EDIT. Generated by sorbet-plugins/rspec/generate_rbi.rb for the rspec Sorbet plugin.'
lines << ''
lines << 'module RSpecConfig'
lines << '  module Everywhere'
lines.concat(module_lines(object_mixins.reverse + added_modules(untyped.ancestors, [untyped]),
                          extra_methods: integration_session_methods(untyped.ancestors)).map { |l| l.delete_prefix('  ') })
lines << '    include DynamicMatchers'
lines << '  end'
lines << ''
lines << '  module DynamicMatchers'
dynamic_matchers.each { |name| lines << "    def #{name}(*args, **kwargs, &blk); end" }
lines << '  end'
lines << ''
lines << '  module Type'
by_type.each do |type, group|
  mods = added_modules(group.ancestors, untyped.ancestors)
  lines << "    module #{camelize(type)}"
  lines.concat(module_lines(mods, extra_methods: integration_session_methods(group.ancestors)))
  lines.concat(route_helper_lines(group.ancestors))
  lines << '    end'
end
lines << '  end'
lines << ''
lines << '  module Extend'
lines << '    module Everywhere'
lines.concat(module_lines(added_modules(untyped.singleton_class.ancestors, [untyped.singleton_class] + BASE.singleton_class.ancestors)))
lines << '      # The plugin turns `[...].each { it ... }` into Sorbet\'s test_each, which Sorbet still type-checks as a call.'
lines << '      def test_each(iterable, &blk); end'
lines << '      def test_each_hash(iterable, &blk); end'
lines << '    end'
lines << ''
lines << '    module Type'
by_type.each do |type, group|
  mods = added_modules(group.singleton_class.ancestors, untyped.singleton_class.ancestors)
  lines << "      module #{camelize(type)}"
  lines.concat(module_lines(mods).map { |l| "  #{l}" })
  lines << '      end'
end
lines << '    end'
lines << '  end'
lines << 'end'

File.write(output, "#{lines.join("\n")}\n")
puts "Wrote #{types.size} spec types and #{dynamic_matchers.size} dynamic matchers to #{output}"
