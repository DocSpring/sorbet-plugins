# typed: true
class Book < ApplicationRecord
  T.unsafe(self).include PhotoUploader.attachment(:cover)
  include CoverOverride
end

T.reveal_type(Book.new.cover_url) # error: Revealed type: `Integer`
T.reveal_type(Book.new.cover) # error: Revealed type: `T.nilable(T.any(T::Hash[Symbol, PhotoUploader::UploadedFile], PhotoUploader::UploadedFile))`
