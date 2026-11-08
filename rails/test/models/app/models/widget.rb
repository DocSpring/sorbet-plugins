# typed: true
class Widget < ApplicationRecord
  include HasKind

  STATUSES = { draft: 0, published: 1 }.freeze
  SHADES = Palette::SHADES

  scope :visible, -> {}
  scope 'named', ->(name) {}
  attr_enum :status, STATUSES
  attr_enum :stage, { 'first' => 0, 'second' => 1 }, prefix: :at, default: nil
  enum :color, %i[red green], suffix: true
  enum :size, { small: 0, large: 1 }, prefix: false, scopes: false
  enum :mode, { on: 1, off: 0 }, instance_methods: false
  enum :shade, SHADES
  enum :tone, Palette::TONES, prefix: true
  suffixed_enum :finish, { matte: 0, gloss: 1 }
  enum :rank, { low: 0, high: 'high' }
end

T.reveal_type(Widget.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(Widget.where(name: 'x')) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.where(name: 'x').first) # error: Revealed type: `T.nilable(Widget)`
T.reveal_type(Widget.find(1)) # error: Revealed type: `Widget`
T.reveal_type(Widget.new) # error: Revealed type: `Widget`
# Overloads, as in Tapioca's RBI: one record, or an array of them.
T.reveal_type(Widget.create(name: 'x')) # error: Revealed type: `Widget`
T.reveal_type(Widget.create([{ name: 'x' }])) # error: Revealed type: `T::Array[Widget]`
T.reveal_type(Widget.first) # error: Revealed type: `T.nilable(Widget)`
T.reveal_type(Widget.first(2)) # error: Revealed type: `T::Array[Widget]`
Widget.new.to_ary # error: Non-private call to private method `to_ary` on `Widget`

# Scopes: on the class, its relations and its association relations.
T.reveal_type(Widget.visible) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.where(name: 'x').named('x')) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(T.let(T.unsafe(nil), Widget::PrivateAssociationRelation).visible) # error: Revealed type: `Widget::PrivateAssociationRelation`

# Enums: the mapping, a predicate and a bang method for each value, and two scopes.
T.reveal_type(Widget.statuses) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.new.status_draft?) # error: Revealed type: `T::Boolean`
Widget.new.status_published!
T.reveal_type(Widget.status_draft) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.not_status_published) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.stages) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.new.at_first?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.colors) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.red_color) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.new.green_color?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.new.small?) # error: Revealed type: `T::Boolean`
Widget.small # error: Method `small` does not exist on `T.class_of(Widget)`
T.reveal_type(Widget.not_on) # error: Revealed type: `Widget::PrivateRelation`
Widget.new.on? # error: Method `on?` does not exist on `Widget`
T.reveal_type(Widget.shades) # error: Revealed type: `T::Hash[T.any(String, Symbol), String]`
T.reveal_type(Widget.new.dark?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.tones) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.new.tone_cool?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.finishes) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.new.matte_finish?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.ranks) # error: Revealed type: `T::Hash[T.any(String, Symbol), T.any(Integer, String)]`

# From the concern.
T.reveal_type(Widget.kinds) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(Widget.new.kind_large?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.recent) # error: Revealed type: `Widget::PrivateRelation`
T.reveal_type(Widget.new.settings_theme) # error: Revealed type: `T.untyped`
Widget.new.settings_font = 'x'
T.reveal_type(Widget.new.settings_theme_changed?) # error: Revealed type: `T::Boolean`
T.reveal_type(Widget.new.saved_change_to_settings_font?) # error: Revealed type: `T::Boolean`

# Columns, with the enums' types: `attr_enum` makes the type an integer, and an attribute with no column isn't nilable.
T.reveal_type(Widget.new.name) # error: Revealed type: `T.nilable(String)`
T.reveal_type(Widget.new.order) # error: Revealed type: `Integer`
T.reveal_type(Widget.new.status) # error: Revealed type: `String`
Widget.new.status = nil # error: Assigning a value to `value` that does not match expected type `T.any(String, Symbol, Integer)`
T.reveal_type(Widget.new.shade) # error: Revealed type: `T.nilable(String)`
Widget.new.shade = 1 # error: Assigning a value to `value` that does not match expected type `T.nilable(T.any(String, Symbol))`
T.reveal_type(Widget.new.kind) # error: Revealed type: `String`
T.reveal_type(Widget.new.settings) # error: Revealed type: `T.untyped`
T.reveal_type(Widget.new.id_value) # error: Revealed type: `Integer`
