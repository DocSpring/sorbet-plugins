# typed: true
class Widget
  include LengthCaps
  include Flags
end

T.reveal_type(Widget.length_caps) # error: Revealed type: `T.untyped`
Widget.length_caps = {}
T.reveal_type(Widget.length_caps?) # error: Revealed type: `T.untyped`
T.reveal_type(Widget.caps) # error: Revealed type: `T.untyped`
T.reveal_type(Widget.new.length_caps) # error: Revealed type: `T.untyped`
T.reveal_type(Widget.new.length_caps?) # error: Revealed type: `T.untyped`
Widget.new.length_caps = {} # error: Setter method `length_caps=` does not exist on `Widget`

Widget.alpha = 1
Widget.beta = 2
Widget.new.alpha = 3
Widget.new.beta?
Widget.hidden = true
Widget.hidden?
Widget.new.hidden # error: Method `hidden` does not exist on `Widget`
Widget.new.hidden = 1
Widget.new.hidden? # error: Method `hidden?` does not exist on `Widget`
Widget.plain = 1
Widget.plain? # error: Method `plain?` does not exist on `T.class_of(Widget)`
Widget.new.plain
Widget.new.plain? # error: Method `plain?` does not exist on `Widget`
