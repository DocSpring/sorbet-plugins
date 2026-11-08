# typed: true
class Gallery < ApplicationRecord
  NAME = :cover

  T.unsafe(self).include PhotoUploader.attachment(NAME) # error: can't read this attachment: the plugin reads `Uploader.attachment(:name)` with a name that's a symbol or string, and no options
  T.unsafe(self).include PhotoUploader.attachment(:cover, store: :other) # error: can't read this attachment
  T.unsafe(self).include Missing.attachment(:file) # error: Unable to resolve constant `Missing` # error: can't type the attachment `file`: its uploader isn't a class in the app's files
  T.unsafe(self).include NotAnUploader.attachment(:file) # error: can't type the attachment `file`: `NotAnUploader` isn't an uploader: a class whose superclasses up to Shrine are the app's
  T.unsafe(self).include UnknownUploader.attachment(:file) # error: can't type the attachment `file`: app/uploaders/unknown_uploader.rb line 3: can't read this `plugin`: doesn't know Shrine's `signature` plugin
  # Sorbet takes only a constant, but the plugin reads this form too.
  include PhotoUploader.attachment(:poster) # error: `include` must only contain constant literals
end

T.reveal_type(Gallery.new.poster_url) # error: Revealed type: `T.nilable(String)`
Gallery.new.file_url # error: Method `file_url` does not exist on `Gallery`
