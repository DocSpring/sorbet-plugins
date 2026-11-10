# typed: true
# An STI subclass declares its superclass's attachment again, with an uploader without derivatives: its own wins.
class Poster < Photo
  T.unsafe(self).include PhotoUploader.attachment(:image)
end

T.reveal_type(Poster.new.image) # error: Revealed type: `T.nilable(T.any(T::Hash[Symbol, PhotoUploader::UploadedFile], PhotoUploader::UploadedFile))`
Poster.new.image(:small) # error: Too many arguments provided for method `Poster::GeneratedShrineAttachmentMethods_image#image`. Expected: `0`, got: `1`
T.reveal_type(Poster.new.thumbnail) # error: Revealed type: `T.nilable(ThumbnailUploader::UploadedFile)`
