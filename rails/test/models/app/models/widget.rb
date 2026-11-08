# typed: true
class Widget < ApplicationRecord
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
