# typed: true
class Album < ApplicationRecord
  include HasCover
end

album = Album.new
# A plugin's `configure` sets `opts[:versions]`: the reader can return a hash of versions. Without the derivatives
# plugin it takes no arguments.
T.reveal_type(album.cover) # error: Revealed type: `T.nilable(T.any(T::Hash[Symbol, PhotoUploader::UploadedFile], PhotoUploader::UploadedFile))`
album.cover(:small) # error: Too many arguments provided for method `Album::GeneratedShrineAttachmentMethods_cover#cover`. Expected: `0`, got: `1`
album.cover_derivatives # error: Method `cover_derivatives` does not exist on `Album`
