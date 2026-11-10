# typed: true
# The attachment comes from HasCover's `included` block, which runs where HasCover is included: before CoverOverride.
class Magazine < ApplicationRecord
  include HasCover
  include CoverOverride
end

T.reveal_type(Magazine.new.cover_url) # error: Revealed type: `Integer`
