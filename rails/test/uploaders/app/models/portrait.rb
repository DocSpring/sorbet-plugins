# typed: true
# An STI subclass has its superclass's attachments, without the methods it or the superclass defines.
class Portrait < Photo
  def image_attacher(**options)
    super
  end
end

T.reveal_type(Portrait.new.image) # error: Revealed type: `T.nilable(ImageUploader::UploadedFile)`
T.reveal_type(T.let(Portrait.new, Portrait::GeneratedShrineAttachmentMethods).image_changed?) # error: Revealed type: `T::Boolean`
T.let(Portrait.new, Portrait::GeneratedShrineAttachmentMethods).image_attacher # error: Method `image_attacher` does not exist on `Portrait::GeneratedShrineAttachmentMethods`
T.let(Portrait.new, Portrait::GeneratedShrineAttachmentMethods).image_url # error: Method `image_url` does not exist on `Portrait::GeneratedShrineAttachmentMethods`
