# typed: true
# The same attachment twice in one class: the second include wins.
class Twin < ApplicationRecord
  T.unsafe(self).include ImageUploader.attachment(:image)
  T.unsafe(self).include PhotoUploader.attachment(:image)
end

T.reveal_type(Twin.new.image) # error: Revealed type: `T.nilable(T.any(T::Hash[Symbol, PhotoUploader::UploadedFile], PhotoUploader::UploadedFile))`
