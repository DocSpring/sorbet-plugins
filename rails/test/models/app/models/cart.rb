# typed: true
class Cart < ApplicationRecord
  # The class's own name, from the top level: itself.
  belongs_to :parent, class_name: 'Cart'
end

T.reveal_type(Cart.new.parent) # error: Revealed type: `T.nilable(Cart)`
