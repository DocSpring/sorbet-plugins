# typed: true
class AccountMailer < ApplicationMailer
  extend T::Sig

  sig { params(account_id: Integer, preview: T::Boolean).returns(T.untyped) }
  def trial_ending(account_id, preview: false); end

  def welcome(user, options = {}); end

  # Anonymous parameters are copied under the names Sorbet gives them.
  def forward(id, *, **, &); end

  def self.class_level; end

  private def inline_private; end

  def made_private; end
  private :made_private

  protected

  def guarded; end

  public

  def public_again; end

  private

  def load_account; end
end

T.reveal_type(AccountMailer.trial_ending(1)) # error: Revealed type: `ActionMailer::MessageDelivery`
AccountMailer.trial_ending('1') # error: Expected `Integer` but found `String("1")` for argument `account_id`
AccountMailer.trial_ending(1, preview: 'yes') # error: Expected `T::Boolean` but found `String("yes")` for argument `preview`
T.reveal_type(AccountMailer.welcome(User.new)) # error: Revealed type: `ActionMailer::MessageDelivery`
AccountMailer.welcome # error: Not enough arguments provided for method `AccountMailer.welcome`
T.reveal_type(AccountMailer.forward(1, 2, 3, key: 4) { nil }) # error: Revealed type: `ActionMailer::MessageDelivery`
T.reveal_type(AccountMailer.class_level) # error: Revealed type: `T.untyped`
AccountMailer.inline_private # error: Method `inline_private` does not exist on `T.class_of(AccountMailer)`
AccountMailer.made_private # error: Method `made_private` does not exist on `T.class_of(AccountMailer)`
AccountMailer.guarded # error: Method `guarded` does not exist on `T.class_of(AccountMailer)`
T.reveal_type(AccountMailer.public_again) # error: Revealed type: `ActionMailer::MessageDelivery`
AccountMailer.load_account # error: Method `load_account` does not exist on `T.class_of(AccountMailer)`
# Inherited from ApplicationMailer, which has Greeting's.
T.reveal_type(AccountMailer.greet(Greeting::Style.new)) # error: Revealed type: `ActionMailer::MessageDelivery`
