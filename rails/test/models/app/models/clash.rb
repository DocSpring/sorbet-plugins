# typed: true
# A concern's `included` block and the class body both change `kind`, and declare `author` differently: the plugin
# can't tell which runs first.
class Clash < ApplicationRecord # error: app/models/concerns/has_kind.rb line 11: can't type `kind`: `Clash` changes its type too # error: app/models/concerns/has_author.rb line 8: can't type `author`: `Clash` declares it too
  include HasKind
  include HasAuthor

  self.table_name = 'widgets'

  attribute :kind, :string
  belongs_to :author, class_name: 'Category'
end
