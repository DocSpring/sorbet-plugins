# typed: true
# Attachments: the methods that the plugins of the uploader, its superclasses and Shrine's global ones define.
class Photo < ApplicationRecord
  T.unsafe(self).include ImageUploader.attachment(:image)
  T.unsafe(self).include ThumbnailUploader::Attachment(:thumbnail)

  # Its own: the attachment's is left out.
  def image_url(*args)
    super
  end
end

photo = Photo.new
# The derivatives plugin: the reader takes a derivative's name.
T.reveal_type(photo.image) # error: Revealed type: `T.nilable(ImageUploader::UploadedFile)`
T.reveal_type(photo.image(:small)) # error: Revealed type: `T.nilable(ImageUploader::UploadedFile)`
T.reveal_type(photo.image_attacher) # error: Revealed type: `ImageUploader::Attacher`
T.reveal_type(photo.image_attacher(validate: false)) # error: Revealed type: `ImageUploader::Attacher`
T.reveal_type(photo.image_changed?) # error: Revealed type: `T::Boolean`
T.reveal_type(photo.image = nil) # error: Revealed type: `NilClass`
T.reveal_type(photo.cached_image_data) # error: Revealed type: `T.nilable(String)`
T.reveal_type(photo.image_derivatives!(:small)) # error: Revealed type: `T.untyped`
T.reveal_type(photo.image_derivatives) # error: Revealed type: `T.untyped`
T.reveal_type(photo.thumbnail_url(:small, public: true)) # error: Revealed type: `T.nilable(String)`
T.reveal_type(photo.thumbnail_attacher) # error: Revealed type: `ThumbnailUploader::Attacher`
T.reveal_type(photo.thumbnail) # error: Revealed type: `T.nilable(ThumbnailUploader::UploadedFile)`
T.let(photo, Photo::GeneratedShrineAttachmentMethods_image).image_url # error: Method `image_url` does not exist on `Photo::GeneratedShrineAttachmentMethods_image`
