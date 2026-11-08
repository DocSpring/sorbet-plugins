# typed: true
module Shop
  class Item < ApplicationRecord
    self.table_name = 'shop_items'
  end
end

T.reveal_type(Shop::Item.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(Shop::Item.where(id: 1)) # error: Revealed type: `Shop::Item::PrivateRelation`
