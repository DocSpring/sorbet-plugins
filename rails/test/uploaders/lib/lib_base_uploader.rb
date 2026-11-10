# typed: true
# An uploader in lib/: Zeitwerk autoloads lib/ too, so it's an uploader as one in app/ is.
class LibBaseUploader < Shrine
  plugin :store_dimensions
end

LibBaseUploader::UploadedFile.new.width
