# typed: true
# As Sidekiq's RBI has it: including Sidekiq::Job extends its untyped ClassMethods, which Sorbet ranks above any
# module the job extends itself.
module Sidekiq
  module Job
    extend T::Helpers

    module ClassMethods
      def perform_async(*args); end
      def perform_in(interval, *args); end
      def perform_at(interval, *args); end
    end

    mixes_in_class_methods(ClassMethods)
  end

  Worker = Job
end
module ActiveJob
  class Base; end
end
module ActiveSupport
  class Duration; end
  class TimeWithZone; end
end
module ActionMailer
  class MailDeliveryJob < ActiveJob::Base; end
end
