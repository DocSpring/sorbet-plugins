# typed: true
# An STI subclass: the base class's table, and its scopes, enums and associations.
class SpecialWidget < Widget
  belongs_to :owner, class_name: 'Person'
  has_many :commenters, through: :comments, source: :person
end

T.reveal_type(SpecialWidget.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(SpecialWidget.where(name: 'x')) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.first) # error: Revealed type: `T.nilable(SpecialWidget)`
T.reveal_type(SpecialWidget.visible) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.status_draft) # error: Revealed type: `SpecialWidget::PrivateRelation`
T.reveal_type(SpecialWidget.statuses) # error: Revealed type: `T::Hash[T.any(String, Symbol), Integer]`
T.reveal_type(SpecialWidget.new.status_draft?) # error: Revealed type: `T::Boolean`
T.reveal_type(SpecialWidget.new.status) # error: Revealed type: `String`
T.reveal_type(SpecialWidget.new.kind) # error: Revealed type: `String`

# Associations: the base class's too. Its module has the change predicates of only its own `belongs_to`s, as Rails
# defines them in the module of the class that declares them.
T.reveal_type(SpecialWidget.new.category) # error: Revealed type: `T.nilable(Category)`
T.reveal_type(SpecialWidget.new.category_changed?) # error: Revealed type: `T::Boolean`
T.let(T.unsafe(nil), SpecialWidget::GeneratedAssociationMethods).category_changed? # error: Method `category_changed?` does not exist on `SpecialWidget::GeneratedAssociationMethods`
T.reveal_type(T.let(T.unsafe(nil), SpecialWidget::GeneratedAssociationMethods).category) # error: Revealed type: `T.nilable(Category)`
T.reveal_type(T.let(T.unsafe(nil), SpecialWidget::GeneratedAssociationMethods).owner_changed?) # error: Revealed type: `T::Boolean`
T.reveal_type(SpecialWidget.new.commenters) # error: Revealed type: `Person::PrivateCollectionProxy`
