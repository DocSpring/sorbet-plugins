# typed: true
class Comment < ApplicationRecord
  belongs_to :post
  belongs_to :person
  belongs_to :subject, polymorphic: true
end

T.reveal_type(Comment.new.post) # error: Revealed type: `T.nilable(Post)`
