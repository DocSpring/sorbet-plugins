# typed: true
# Overrides an attachment's method: included after the attachment, it wins, as in Ruby.
module CoverOverride
  extend T::Sig

  sig { returns(Integer) }
  def cover_url
    0
  end
end
