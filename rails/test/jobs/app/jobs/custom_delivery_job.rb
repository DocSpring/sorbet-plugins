# typed: true
# A gem superclass: the plugin can't see whether it's a Sidekiq or ActiveJob job.
class CustomDeliveryJob < ActionMailer::MailDeliveryJob # error: rails plugin: can't tell what kind of job `CustomDeliveryJob` is: its superclass `ActionMailer::MailDeliveryJob` isn't in the app. Inherit from ApplicationJob, or include Sidekiq::Job
  def perform(id); end
end
