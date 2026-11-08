# typed: strict
module API
  module V1
    module Concerns
      # instance_accessor: false: class methods only.
      module HasIdempotencyKeys
        extend ActiveSupport::Concern

        included do
          T.bind(self, T::Class[T.anything])
          class_attribute :idempotent_creates, instance_accessor: false, default: {}.freeze
        end
      end
    end
  end
end
