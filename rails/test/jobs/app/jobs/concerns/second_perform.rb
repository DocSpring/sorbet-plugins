# typed: true
module SecondPerform
  extend T::Sig

  sig { params(id: Integer).void }
  def perform(id); end
end
