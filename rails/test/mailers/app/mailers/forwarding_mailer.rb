# typed: true
class ForwardingMailer < ApplicationMailer # error: rails plugin: can't copy the parameters of ForwardingMailer#relay
  def relay(...); end

  def fine(id); end
end

T.reveal_type(ForwardingMailer.fine(1)) # error: Revealed type: `ActionMailer::MessageDelivery`
