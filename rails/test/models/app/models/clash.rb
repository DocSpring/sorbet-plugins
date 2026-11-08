# typed: true
# A concern's `included` block and the class body both change `kind`: the plugin can't tell which runs first.
class Clash < ApplicationRecord # error: app/models/concerns/has_kind.rb line 11: can't type `kind`: `Clash` changes its type too
  include HasKind

  self.table_name = 'widgets'

  attribute :kind, :string
end
