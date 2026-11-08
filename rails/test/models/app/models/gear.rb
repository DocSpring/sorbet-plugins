# typed: true
# The app's own uncountable word.
class Gear < ApplicationRecord
end

T.reveal_type(Gear.ids) # error: Revealed type: `T::Array[Integer]`
