# typed: true
class FailureAlertMailer < ApplicationMailer
  extend T::Sig

  helper_method :failure_time

  sig { params(time: Time, show_zone: T::Boolean).returns(String) }
  def failure_time(time, show_zone: false) = ''
end
