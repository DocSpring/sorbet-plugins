# typed: true
# Not the class's Zeitwerk file (that would be misplaced.rb): it gets nothing here.
class Misplaced < ApplicationRecord
end

Misplaced.ids # error: Method `ids` does not exist on `T.class_of(Misplaced)`
