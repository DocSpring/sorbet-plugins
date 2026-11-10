# typed: true
# A saved file that other jobs read: its `perform` sig has a constant path with nothing after `::`.
module DanglingPerform
  extend T::Sig

  sig { params(a: T::).void } # error: expected constant name following "::"
  def perform(a); end
end
