# typed: true
# A subclass: its Attacher and UploadedFile are its superclass's subclasses, as Shrine.inherited makes them.
class ThumbnailUploader < ImageUploader
  plugin :upload_options
  # Loaded again: its modules are already there, and `versions_compatibility: true` makes its `configure` include
  # another module in the Attacher.
  plugin :derivatives, versions_compatibility: true
end

ThumbnailUploader::Attacher.new.load_versions
T.reveal_type(ThumbnailUploader::Attacher.new.url) # error: Revealed type: `String`

ThumbnailUploader.new.upload_options
ThumbnailUploader.new.extract_dimensions(nil)
T.let(ThumbnailUploader::Attacher.new, ImageUploader::Attacher)
ThumbnailUploader::Attacher.new.derivatives
ThumbnailUploader::UploadedFile.new.width
ImageUploader.new.upload_options # error: Method `upload_options` does not exist on `ImageUploader`
