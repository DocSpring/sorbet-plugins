# typed: strict
# sorbet-args: --enable-experimental-requires-ancestor
# instance_writer: false keeps the instance reader and predicate.
module LengthCaps
  extend T::Sig
  extend ActiveSupport::Concern

  included do
    T.bind(self, T::Class[T.anything])

    class_attribute :length_caps, instance_writer: false, default: {}
  end

  class_methods do
    extend T::Sig
    extend T::Helpers

    requires_ancestor { GeneratedClassMethods }

    sig { returns(T.untyped) }
    def caps = length_caps
  end
end
