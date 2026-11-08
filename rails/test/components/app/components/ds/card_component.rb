# typed: true
# Slots: the methods ViewComponent defines for each, untyped as the app's compiler wrote them.
module DS
  class CardComponent < BaseComponent
    renders_many :actions, ->(**button) { PaginationComponent.new }
    renders_one :footer
    renders_one 'pagination', PaginationComponent
    # The app's irregular plural: `with_cactus`.
    renders_many :cacti

    # A component defined in another's file.
    class SectionComponent < BaseComponent
      renders_one :heading
    end

    # The slots' methods stay public.
    private

    def title; end
  end
end

card = DS::CardComponent.new
T.reveal_type(card.actions) # error: Revealed type: `T::Array[T.untyped]`
T.reveal_type(card.actions?) # error: Revealed type: `T::Boolean`
T.reveal_type(card.with_action(label: 'x') {}) # error: Revealed type: `T.untyped`
T.reveal_type(card.with_action_content('x')) # error: Revealed type: `T.untyped`
T.reveal_type(card.with_actions([{ label: 'x' }]) {}) # error: Revealed type: `T.untyped`
card.with_actions
T.reveal_type(card.footer) # error: Revealed type: `T.untyped`
T.reveal_type(card.footer?) # error: Revealed type: `T::Boolean`
T.reveal_type(card.with_footer) # error: Revealed type: `T.untyped`
T.reveal_type(card.with_footer_content('x')) # error: Revealed type: `T.untyped`
card.with_footers # error: Method `with_footers` does not exist on `DS::CardComponent`
T.reveal_type(card.pagination) # error: Revealed type: `T.untyped`
T.reveal_type(card.with_cactus) # error: Revealed type: `T.untyped`
T.reveal_type(card.with_cacti([])) # error: Revealed type: `T.untyped`
T.reveal_type(DS::CardComponent::SectionComponent.new.heading) # error: Revealed type: `T.untyped`
