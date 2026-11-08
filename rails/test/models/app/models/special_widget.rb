# typed: true
# An STI subclass: the base class's table, and its scopes and enums.
class SpecialWidget < Widget
end

T.reveal_type(SpecialWidget.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(SpecialWidget.where(name: 'x')) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.first) # error: Revealed type: `T.nilable(SpecialWidget)`
T.reveal_type(SpecialWidget.visible) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.status_draft) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.statuses) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(SpecialWidget.new.status_draft?) # error: Revealed type: `T::Boolean`
