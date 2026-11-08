# typed: true
class CustomDeviseMailer < Devise::Mailer
  extend T::Sig

  def devise_mail(record, action, opts = {}, &); end

  sig { params(record: User, token: String, opts: T::Hash[Symbol, T.untyped]).returns(T.untyped) }
  def otp_unlock_instructions(record, token, opts = {}); end
end

T.reveal_type(CustomDeviseMailer.devise_mail(User.new, :confirmation)) # error: Revealed type: `ActionMailer::MessageDelivery`
CustomDeviseMailer.otp_unlock_instructions(User.new, 1) # error: Expected `String` but found `Integer(1)` for argument `token`
T.reveal_type(CustomDeviseMailer.otp_unlock_instructions(User.new, 'token')) # error: Revealed type: `ActionMailer::MessageDelivery`
# The gem parent's actions come from its RBI.
T.reveal_type(CustomDeviseMailer.confirmation_instructions(User.new, 'token')) # error: Revealed type: `T.untyped`
