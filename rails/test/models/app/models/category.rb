# typed: true
class Category < ApplicationRecord
end

T.reveal_type(Category.ids) # error: Revealed type: `T::Array[Integer]`
