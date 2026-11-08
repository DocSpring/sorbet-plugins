# typed: true
class ApplicationMailer < ActionMailer::Base
  include Greeting
  # A gem's module: its public methods are actions at runtime, but nothing sends them, so they're left out.
  include ActionView::Helpers::NumberHelper

  # Not a mailer: it gets no actions.
  class AbortDeliveryError < StandardError
    def details; end
  end

  private

  def layout_name; end
end

T.reveal_type(ApplicationMailer.greet(Greeting::Style.new)) # error: Revealed type: `ActionMailer::MessageDelivery`
ApplicationMailer.greet(1) # error: Expected `Greeting::Style` but found `Integer(1)` for argument `style`
ApplicationMailer.layout_name # error: Method `layout_name` does not exist on `T.class_of(ApplicationMailer)`
ApplicationMailer.number_to_currency(1) # error: Method `number_to_currency` does not exist on `T.class_of(ApplicationMailer)`
ApplicationMailer::AbortDeliveryError.details # error: Method `details` does not exist on `T.class_of(ApplicationMailer::AbortDeliveryError)`
