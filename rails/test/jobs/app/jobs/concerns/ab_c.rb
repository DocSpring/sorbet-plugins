# typed: true
# Its file's name differs from a_bc.rb's only in underscores: Zeitwerk finds each through the inflector.
module AbC
  extend T::Sig

  sig { params(id: Integer).void }
  def perform(id); end
end
