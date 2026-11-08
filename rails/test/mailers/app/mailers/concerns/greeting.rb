# typed: true
# An app module a mailer includes: its public methods are the mailer's actions too.
module Greeting
  extend T::Sig

  class Style; end

  # `Style` resolves in this module's nesting, wherever the action is written.
  sig { params(style: Style).void }
  def greet(style); end

  private

  def secret; end
end
