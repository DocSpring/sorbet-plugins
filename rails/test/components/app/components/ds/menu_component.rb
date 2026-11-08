# typed: true
# Polymorphic slots (`types:`): a pair of setters per type, named by its `as:` or by the slot and the type.
module DS
  class MenuComponent < BaseComponent
    renders_many :items,
                 types: {
                   item: { renders: PaginationComponent, as: :item },
                   header: { renders: ->(label:) { label }, as: 'heading' },
                   divider: -> { nil },
                   custom: { renders: nil },
                   link: PaginationComponent,
                 }
    renders_one :visual, types: { icon: PaginationComponent, avatar: { renders: lambda { |name:| name } } }
    # In braces, the same hash to ViewComponent.
    renders_one :badge, { types: { count: PaginationComponent } }
  end
end

menu = DS::MenuComponent.new
T.reveal_type(menu.items) # error: Revealed type: `T::Array[T.untyped]`
T.reveal_type(menu.items?) # error: Revealed type: `T::Boolean`
T.reveal_type(menu.with_item(href: '/')) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_item_content('x')) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_heading(label: 'x')) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_item_divider) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_item_custom_content('x')) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_item_link) # error: Revealed type: `T.untyped`
menu.with_items([]) # error: Method `with_items` does not exist on `DS::MenuComponent`
menu.with_header # error: Method `with_header` does not exist on `DS::MenuComponent`
T.reveal_type(menu.visual) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_visual_icon) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_visual_avatar_content('x')) # error: Revealed type: `T.untyped`
T.reveal_type(menu.with_badge_count) # error: Revealed type: `T.untyped`
