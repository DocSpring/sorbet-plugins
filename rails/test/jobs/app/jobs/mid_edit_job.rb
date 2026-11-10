# typed: true
# Text the language server sees while someone types: a constant path with nothing after `::`.
class MidEditJob < ParentJob:: # error: expected constant name following "::"
end

class MidEditSigJob
  include Sidekiq::Job
  extend T::Sig

  sig { params(a: T::).void } # error: expected constant name following "::"
  def perform(a); end
end

module MidEdit::Namespace:: # error: expected constant name following "::"
end
