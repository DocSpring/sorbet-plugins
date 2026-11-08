# typed: true
# Several attributes in one call, and each instance_* option.
module Flags
  extend ActiveSupport::Concern

  included do
    T.bind(self, T::Class[T.anything])
    class_attribute :alpha, :beta
    class_attribute :hidden, instance_reader: false
    class_attribute :plain, instance_predicate: false
  end
end
