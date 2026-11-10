# typed: true
module ABc
  extend T::Sig

  sig { params(name: String).void }
  def perform(name); end
end
