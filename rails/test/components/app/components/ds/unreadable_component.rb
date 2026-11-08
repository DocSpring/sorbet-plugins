# typed: true
# Slots the plugin can't read: an error at each, and the rest are still written.
module DS
  class UnreadableComponent < BaseComponent
    TYPES = { link: PaginationComponent }.freeze

    name = :x
    renders_one name # error: can't read this `renders_one`: the plugin reads a slot whose name is a symbol or string
    renders_one :built, PaginationComponent.itself # error: can't read `renders_one :built`: the plugin reads a component, a lambda, a class name or `types:` after the name
    renders_one :braces, { types: TYPES } # error: can't read `renders_one :braces`: the plugin reads `types:` written out as a hash
    renders_one :options, renders: PaginationComponent # error: can't read `renders_one :options`: the plugin reads a component, a lambda, a class name or `types:` after the name
    renders_many :constant_types, types: TYPES # error: can't read `renders_many :constant_types`: the plugin reads `types:` written out as a hash
    renders_many :string_keys, types: { 'link' => PaginationComponent } # error: can't read `renders_many :string_keys`: the plugin reads `types:` with symbol keys
    renders_many :named, types: { link: { renders: PaginationComponent, as: name } } # error: can't read `renders_many :named`: the plugin reads a type's `as:` as a symbol or string (`link`)
    renders_many :called, types: { link: PaginationComponent.itself } # error: can't read `renders_many :called`: the plugin reads a type as a component, a lambda, a class name or `{ renders: ..., as: ... }` (`link`)
    with_options({}) do
      renders_one :nested # error: can't read `renders_one` here: the plugin reads it only directly in a class body
    end
    renders_one :fine
  end
end

T.reveal_type(DS::UnreadableComponent.new.fine) # error: Revealed type: `T.untyped`
