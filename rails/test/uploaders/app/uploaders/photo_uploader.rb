# typed: true
class PhotoUploader < Shrine
  plugin Versioned
end

PhotoUploader.version_names
PhotoUploader::UploadedFile.new.version_name
PhotoUploader::Attacher.new.derivatives # error: Method `derivatives` does not exist on `PhotoUploader::Attacher`
PhotoUploader::Attacher.new.keep_files # error: Method `keep_files` does not exist on `PhotoUploader::Attacher`
