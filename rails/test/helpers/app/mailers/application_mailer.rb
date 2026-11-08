# typed: true
class ApplicationMailer < ActionMailer::Base
  include EmailLayout
  helper MailerHelper

  # Not a mailer: it gets nothing.
  class AbortDeliveryError < StandardError; end
end
