# typed: true
class Tag < ApplicationRecord
  has_and_belongs_to_many :posts
end

T.reveal_type(Tag.new.posts) # error: Revealed type: `Post::PrivateCollectionProxy`
