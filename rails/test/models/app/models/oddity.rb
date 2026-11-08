# typed: true
# A column type the plugin doesn't know is an error, and the other columns are still written.
class Oddity < ApplicationRecord # error: doesn't type the column `cost` of type `money`
end

T.reveal_type(Oddity.new.id) # error: Revealed type: `Integer`
Oddity.new.cost # error: Method `cost` does not exist on `Oddity`
