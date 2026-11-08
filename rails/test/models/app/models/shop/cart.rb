# typed: true
# Rails looks an association's class up in the namespaces of the class that declares it, innermost first, then at the
# top level, and takes the first constant that has exactly that name.
module Shop
  class Cart < ApplicationRecord
    self.table_name = 'shop_carts'

    # `Shop::Item`, ahead of the top-level `Item`.
    has_many :items
    # No `Shop::Category`: the top-level one.
    belongs_to :category
    # A name written with `::` is looked up at the top level only.
    has_one :item, class_name: '::Item'
    # The class's own name: Rails looks for it at the top level first (`_klass`), so this is `::Cart`, not `Shop::Cart`.
    belongs_to :parent, class_name: 'Cart'
    # A namespaced name, looked up in the class's namespaces too.
    has_many :others, class_name: 'Shop::Cart'
  end
end

T.reveal_type(Shop::Cart.new.items) # error: Revealed type: `Shop::Item::PrivateCollectionProxy`
T.reveal_type(Shop::Cart.new.category) # error: Revealed type: `T.nilable(Category)`
T.reveal_type(Shop::Cart.new.item) # error: Revealed type: `T.nilable(Item)`
T.reveal_type(Shop::Cart.new.parent) # error: Revealed type: `T.nilable(Cart)`
T.reveal_type(Shop::Cart.new.others) # error: Revealed type: `Shop::Cart::PrivateCollectionProxy`
