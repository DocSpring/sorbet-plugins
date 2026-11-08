# typed: true
# A concern's `included` block: its macros run in each class that includes it, and its constants are looked up in the
# concern.
module HasKind
  extend ActiveSupport::Concern

  KINDS = { small: 0, large: 1 }.freeze

  included do
    T.bind(self, T.class_of(ApplicationRecord))
    attr_enum :kind, KINDS
    scope :recent, -> {}
    store_accessor :settings, :theme, :font, prefix: true
  end
end
