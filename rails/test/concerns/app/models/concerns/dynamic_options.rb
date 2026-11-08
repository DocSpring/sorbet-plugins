# typed: true
module DynamicOptions
  extend ActiveSupport::Concern

  OPTIONS = T.let({}, T::Hash[Symbol, T.untyped])

  included do
    T.bind(self, T::Class[T.anything])
    class_attribute :dynamic, instance_writer: OPTIONS.empty? # error: rails plugin: can't read this `class_attribute`
  end
end
