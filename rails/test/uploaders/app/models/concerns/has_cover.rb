# typed: true
# An attachment in a concern's `included` block is the model's.
module HasCover
  extend ActiveSupport::Concern

  included do
    T.unsafe(self).include PhotoUploader.attachment(:cover)
  end
end
