# typed: true
module FirstPerform
  extend T::Sig

  sig { params(name: String).void }
  def perform(name); end
end
