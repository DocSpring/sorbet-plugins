# typed: true
# An irregular plural (`people`), and an integer primary key.
class Person < ApplicationRecord
end

T.reveal_type(Person.ids) # error: Revealed type: `T::Array[Integer]`
T.reveal_type(Person.new.id) # error: Revealed type: `Integer`
