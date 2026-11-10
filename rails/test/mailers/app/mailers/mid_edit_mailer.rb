# typed: true
class MidEditMailer < ApplicationMailer
  extend T::Sig

  sig { params(a: T::).void } # error: expected constant name following "::"
  def notice(a); end
end
