# typed: true
# Each `plugin` call mixes in the plugin's modules, one under `unless` too.
class ImageUploader < Shrine
  plugin :derivatives
  plugin :store_dimensions
  plugin KeepFiles unless ENV['TEST']
end

ImageUploader.new.extract_dimensions(nil)
ImageUploader.dimensions_analyzer(:fastimage)
ImageUploader.derivatives_processor
ImageUploader.new.pretty_location(nil)
T.let(ImageUploader::Attacher.new, Shrine::Attacher)
ImageUploader::Attacher.new.derivatives
ImageUploader::Attacher.derivatives(:thumbnails) {}
ImageUploader::Attacher.new.keep_files
ImageUploader::UploadedFile.new.width
ImageUploader::UploadedFile.new.derivative_url

# The plugins a Shrine plugin loads come first: derivatives loads default_url, so its `url` is derivatives'.
ImageUploader::Attacher.default_url {}
T.reveal_type(ImageUploader::Attacher.new.url) # error: Revealed type: `String`
ImageUploader::Attacher.new.load_versions # error: Method `load_versions` does not exist on `ImageUploader::Attacher`

# The global plugins are on Shrine's own classes, which the gem's RBIs type.
ImageUploader::Attacher.new.activerecord_after_save # error: Method `activerecord_after_save` does not exist on `ImageUploader::Attacher`
