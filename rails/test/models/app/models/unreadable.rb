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
  end
  scope :fine, -> {}
end

T.reveal_type(Unreadable.fine) # error: Revealed type: `Unreadable::PrivateRelation`
