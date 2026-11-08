# typed: true
# Macros the plugin can't read: an error at each, and the rest are still written.
class Unreadable < ApplicationRecord # error: app/models/concerns/bad_macros.rb line 9: can't read this `scope`
  include BadMacros

  self.table_name = 'widgets'

  def self.computed; end

  name = :x
  scope name, -> {} # error: can't read this `scope`: the plugin reads a scope whose name is a symbol or string
  enum :a, computed # error: can't read the values of `enum :a`
  enum :b, MISSING # error: Unable to resolve constant `MISSING` # error: can't find the constant `MISSING` (the values of `enum :b`)
  enum :c, { 'not a name': 0 } # error: `not a name` isn't a method name
  enum :d, { x: 0 }, prefix: computed # error: can't read `prefix:` of `enum :d`
  with_options({}) do
    scope :nested, -> {} # error: can't read `scope` here: the plugin reads it only directly in a class body
    attribute :nested_attribute, :string # error: can't read `attribute` here: the plugin reads it only directly in a class body
  end
  scope :fine, -> {}

  attribute name # error: can't read this `attribute`: the plugin reads a name that's a symbol or string
  attribute :typed, ActiveRecord::Type::Binary.new # error: can't read the type of `attribute :typed`: the plugin reads a type that's a symbol (`:boolean`)
  attribute :money, :money # error: doesn't type `attribute :money, :money`
  serialize :coded, coder: computed # error: can't read `coder:` of `serialize :coded`
  alias_attribute :other, computed # error: can't read this `alias_attribute`
  self.ignored_columns += computed # error: can't read `self.ignored_columns +=`
  text_attribute :note # error: can't read `text_attribute`'s definition: it calls `attribute`
  attribute :fine_column, :integer
  enum :ghost, { x: 0 } # error: `enum :ghost` has no column or type
end

T.reveal_type(Unreadable.fine) # error: Revealed type: `Unreadable::PrivateRelation`
T.reveal_type(Unreadable.new.fine_column) # error: Revealed type: `Integer`
T.reveal_type(Unreadable.new.name) # error: Revealed type: `T.nilable(String)`
