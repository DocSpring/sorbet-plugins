# typed: true
# An STI subclass: the base class's table.
class SpecialWidget < Widget
end

T.reveal_type(SpecialWidget.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(SpecialWidget.where(name: 'x')) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.first) # error: Revealed type: `T.nilable(SpecialWidget)`
