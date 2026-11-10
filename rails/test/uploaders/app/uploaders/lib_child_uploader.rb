# typed: true
# Its superclass is in lib/.
class LibChildUploader < LibBaseUploader
  plugin :upload_options
end

LibChildUploader.new.upload_options
LibChildUploader::UploadedFile.new.width
T.let(LibChildUploader::Attacher.new, LibBaseUploader::Attacher)
