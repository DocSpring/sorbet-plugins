# frozen_string_literal: true

require 'prism'
require 'set'
require_relative 'partial_locals'
require_relative 'template_ruby'

# Checks, each time one of the app's views renders, that its instance variables and locals have the types Sorbet has
# for them: its RBI's (sorbet/rbi/erb_views/app/views/<template>.rbi: inferred from the controllers, or an ivars
# comment's), and a partial's sig comment's. Sorbet trusts those types, so one that a real render contradicts hides
# bugs; the test suite renders the templates, so a wrong inference or a wrong declaration fails a spec. Load it in the
# test environment:
#
#   require File.join(Gem::Specification.find_by_name('sorbet').gem_dir, 'lib/sorbet-plugins/erb_views/generator/runtime_check')
#
# It reads a template's types the first time it renders. The check is shallow (an array's class, not its elements),
# so it never loads a relation. A type with a constant that exists only in Tapioca's RBIs (a model's
# `PrivateRelation`) goes unchecked. A view spec assigns the ivars itself, where the app's controllers and mailers
# would, so there it checks only the locals. A component's template has its component's ivars, which Sorbet checks.
module ErbViews
  module RuntimeCheck
    class TypeError < StandardError; end

    RBI_ONLY = /\A(Private|Generated)\w*\z|\ACommonRelationMethods\z/

    # How generator/rbi.rb writes an ivar's type, and a local's for a template without a locals comment.
    IVAR = /^ +(@\w+) = T\.let\(T\.unsafe\(nil\), (.+)\)$/
    LOCAL = /^ +sig \{ returns\((.+)\) \}\n +def ([a-z_]\w*); end$/

    # name => type (Ruby text) for a template's ivars and locals, and where each kind is declared.
    Types = Struct.new(:ivars, :locals, :ivars_from, :locals_from, keyword_init: true)

    class << self
      attr_writer :root, :rbi_dir

      def root = @root ||= Rails.root.to_s

      def rbi_dir = @rbi_dir ||= File.join(root, 'sorbet/rbi/erb_views')

      def check(template, view, locals)
        path = template.identifier.delete_prefix("#{root}/")
        types = types_for(path)
        return if types.nil?

        unless view_spec?(view)
          types.ivars.each { |name, type| check_value(path, name, view.instance_variable_get(name), type, types.ivars_from) }
        end
        types.locals.each do |name, type|
          check_value(path, name, locals[name], type, types.locals_from) if locals.key?(name)
        end
      end

      # nil for a template that isn't one of the app's views.
      def types_for(path)
        @types ||= {}
        @types.fetch(path) { @types[path] = read_types(path) }
      end

      private

      def read_types(path)
        return nil unless path.start_with?('app/views/') && path.end_with?('.erb') && File.file?(File.join(root, path))

        source = File.read(File.join(root, path))
        rbi_file = File.join(rbi_dir, "#{path}.rbi")
        rbi = File.file?(rbi_file) ? File.read(rbi_file) : ''
        rbi_name = rbi_file.delete_prefix("#{root}/")
        # One the template assigns itself may be unset when it starts.
        written = ivar_writes(Prism.parse(TemplateRuby.call(source)).value)
        ivars = rbi.scan(IVAR).to_h do |name, type|
          [name.to_sym, written.include?(name.to_sym) && !type.start_with?('T.nilable(') ? "T.nilable(#{type})" : type]
        end
        declared = PartialLocals.read(source)
        locals = if declared
                   declared.params.select(&:type).to_h { |param| [param.name.to_sym, param.type] }
                 else
                   rbi.scan(LOCAL).to_h { |type, name| [name.to_sym, type] }
                 end
        Types.new(ivars: ivars.reject { |_, type| type == 'T.untyped' }, locals: locals.reject { |_, type| type == 'T.untyped' },
                  ivars_from: rbi_name, locals_from: declared ? 'its sig comment' : rbi_name)
      end

      def ivar_writes(node, found = Set.new)
        case node
        when Prism::InstanceVariableWriteNode, Prism::InstanceVariableTargetNode, Prism::InstanceVariableOrWriteNode,
             Prism::InstanceVariableAndWriteNode, Prism::InstanceVariableOperatorWriteNode
          found << node.name
        end
        node.compact_child_nodes.each { |child| ivar_writes(child, found) }
        found
      end

      def view_spec?(view)
        defined?(ActionView::TestCase::TestController) && view.respond_to?(:controller) &&
          view.controller.is_a?(ActionView::TestCase::TestController)
      end

      def check_value(path, name, value, type, from)
        resolved = resolve(type)
        return if resolved.nil? || resolved.valid?(value)

        raise TypeError, "#{path} rendered with #{name} = #{value.class}, which isn't a #{resolved.name}, as #{from} " \
                         'declares. Fix the code that renders it, or the type: an ivars or sig comment\'s, or what the ' \
                         'erb_views generator infers (internal_docs/sorbet.md).'
      end

      def resolve(type)
        @resolved ||= {}
        @resolved.fetch(type) do
          @resolved[type] = begin
            T::Utils.coerce(TOPLEVEL_BINDING.eval(type))
          rescue NameError => e
            # Tapioca's RBI-only classes: the relation and association classes it generates for each model.
            raise unless !e.is_a?(NoMethodError) && e.name.to_s.match?(RBI_ONLY)

            nil
          end
        end
      end
    end

    module Template
      def render(view, locals, ...)
        RuntimeCheck.check(self, view, locals)
        super
      end
    end
  end
end

ActionView::Template.prepend(ErbViews::RuntimeCheck::Template)
