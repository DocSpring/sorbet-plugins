# typed: true
class Item < ApplicationRecord
end

T.reveal_type(Item.ids) # error: Revealed type: `T::Array[Integer]`
