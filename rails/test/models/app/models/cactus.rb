# typed: true
# The app's own irregular plural.
class Cactus < ApplicationRecord
end

T.reveal_type(Cactus.ids) # error: Revealed type: `T::Array[Integer]`
