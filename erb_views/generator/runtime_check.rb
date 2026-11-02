# frozen_string_literal: true

# Checks, each time a template renders, that its instance variables and locals have the types its context in
# sorbet/rbi/plugins/erb_views.rbi declares: the types inferred from the controllers, an ivars comment's, a partial's
# sig comment's. Sorbet trusts those types, so one that a real render contradicts hides bugs; the test suite renders
# the templates, so a wrong inference or a wrong declaration fails a spec. The check is shallow (an array's class,
# not its elements), so it never loads a relation. A type with a constant that exists only in Tapioca's RBIs (a
# model's `PrivateRelation`) goes unchecked. The generator copies this file into the one it writes with the types
# (Rbi#runtime_check).
module ErbViews
  module RuntimeCheck
    class TypeError < StandardError; end

    RBI_ONLY = /\A(Private|Generated)\w*\z|\ACommonRelationMethods\z/

    class << self
      # template path (relative to the app) => { ivars: { :@name => -> { type } }, locals: { name: -> { type } } }
      attr_accessor :types

      def check(template, view, locals)
        expected = types[template.identifier.delete_prefix("#{Rails.root}/")]
        return if expected.nil?

        # A view spec assigns the ivars itself, where the app's controllers and mailers would.
        unless view_spec?(view)
          expected.fetch(:ivars).each do |name, type|
            check_value(template, name, view.instance_variable_get(name), type)
          end
        end
        expected.fetch(:locals).each do |name, type|
          check_value(template, name, locals[name], type) if locals.key?(name)
        end
      end

      private

      def view_spec?(view)
        defined?(ActionView::TestCase::TestController) && view.respond_to?(:controller) &&
          view.controller.is_a?(ActionView::TestCase::TestController)
      end

      def check_value(template, name, value, type)
        resolved = resolve(type)
        return if resolved.nil? || resolved.valid?(value)

        path = template.identifier.delete_prefix("#{Rails.root}/")
        raise TypeError, "#{path} rendered with #{name} = #{value.class}, which isn't a #{resolved.name}, as its context " \
                         'declares (sorbet/rbi/plugins/erb_views.rbi). Fix the code that renders it, or the type: an ivars ' \
                         "or sig comment's, or what the generator infers (internal_docs/sorbet.md)."
      end

      def resolve(type)
        @resolved ||= {}.compare_by_identity
        @resolved.fetch(type) do
          @resolved[type] = begin
            T::Utils.coerce(type.call)
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
